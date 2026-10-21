#pragma once

// Panorama JS for CAMERA EDITOR MODE: a docked editor workspace built ONCE into the
// in-game HUD (CSGOHud) context by CameraEditorHud.cpp.
//
// The chrome frames the live game as a "preview" in the top-left and fills the rest of
// the screen with editor panels:
//   * a RIGHT inspector column (selected-camera/property controls), and
//   * a BOTTOM backdrop behind the existing CameraTimeline panel (which renders in its
//     own root at a higher z-index, so it sits on top of this backdrop).
// The preview frame emerges automatically from the inspector's left accent border and
// the backdrop's top accent border (the backdrop border is clipped by the inspector
// where they meet, giving an L-shaped frame around the top-left preview rect).
//
// The gameplay HUD hide lives in CameraTimelineJs (driven by its "hosted" state flag),
// so a single script owns native-HUD visibility; this script only draws chrome.
//
// All controls issue the EXISTING "mirv_filmmaker camtl ... / marker ..." console
// commands -- the same back-end as the hotkeys and the timeline. Per the Panorama input
// constraint (in-game HUD JS has no mouse-move event) every draggable control is a
// native Slider panel.
//
//   C++ -> JS : attribute "state" (camera readouts + selected-key settings), then
//               $.CamEditor.render().
//   JS  -> C++: buttons / sliders issue console commands.

namespace Filmmaker {

inline const char* kCameraEditorJs = R"EDJS(
(function () {
  try {
    var existing = $('#CamEditorRoot'); if (existing) existing.DeleteAsync(0);
    var existingDbg = $('#CamEditorDebugRoot'); if (existingDbg) existingDbg.DeleteAsync(0);
    var existingRender = $('#CamEditorRenderRoot'); if (existingRender) existingRender.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#f0b323ff', freeze: '#80bdffff', render: '#aeb4bcff', danger: '#c92a2acc',
      bg: '#0c0e12ff', bgSoft: '#14181eff',
      panelBorder: '#ffffff1f', sectionBg: '#1a1f26ff',
      label: '#c8d3dfff', value: '#eef2f6ff', dim: '#6b7480ff',
      // TEXT TIERS, measured in APCA Lc against sectionBg/bgRaised (the two worst backdrops):
      //   value    Lc 97  -- the number/name you came to read
      //   label    Lc 75  -- row labels. Was #9aa4b0 (Lc 50) and these run at 11-14px.
      //   hint     Lc 60  -- STATIC supporting text: section headers, hints, notes, unit marks.
      //   inactive Lc 60  -- de-emphasized STATE: the off half of a value/accent pair.
      //   dim      Lc 27  -- DISABLED CONTROLS ONLY. Below the readable floor on purpose.
      // `dim` used to serve all three of the last roles at Lc 27 -- under the floor for text at
      // any size. That hid state the user needs in order to operate a panel (which channel is
      // live, whether a follow target resolved, what the multiplier row is waiting for) AND it
      // hid instructions ('Place a camera and select a target.' at 11px).
      //   toggles against S.value/S.accent to encode state -> `inactive`
      //   always quiet, never toggles                      -> `hint`
      //   the control is genuinely inert                    -> `dim`, and drop its FILL at the
      //     same time (see custApplyBtn / btnVisual) so "disabled" reads as disabled rather than
      //     as an enabled button whose text went missing.
      // hint and inactive deliberately share a value: both want "quiet but readable", and they
      // are kept as separate names so ~50 call sites document which of the two they mean. Do not
      // collapse them into one token -- if the state tier ever needs to diverge (a different hue,
      // an italic), it has to be changeable without touching every static caption in the editor.
      hint: '#bcc6d2ff', inactive: '#bcc6d2ff',   // APCA Lc 71 on #0c0e12
      btnBg: '#ffffff14', btnOn: '#f0b32333', renderOn: '#aeb4bc26',
      // Redesign tokens (ADDITIVE -- the keys above are load-bearing across every #include'd
      // fragment; never rename/remove them). Elevation tiers, button hierarchy, slider tracks.
      bgRaised: '#232a35ff',                       // active/selected surfaces (3rd elevation tier)
      cardBorder: '#ffffff10',
      cardShadow: '#00000066 0px 3px 12px 0px',    // section cards sit ON the inspector bg
      accentText: '#151004ff',                     // dark text on a gold PRIMARY fill
      accentHover: '#ffc93dff',                    // hover fill for a SOLID gold button. btnVisual's
                                                   // default hover (S.btnHover) is a translucent
                                                   // white that WASHES a gold fill out -- see the
                                                   // __bgHover override in CameraEditorWidgetsJs.h
      accentDown: '#d99c14ff',                     // and its pressed counterpart
      dangerBg: '#c92a2a26', dangerBorder: '#c92a2a66', dangerText: '#ff9b96ff',
      trackBg: '#00000066',
      // Interaction tiers. Panorama inline styles have no :hover/:active and this in-game HUD
      // context has no stylesheet of ours to hang classes on, so hover/press are driven from
      // panel events through btnVisual() in CameraEditorWidgetsJs.h. These are the fills it picks.
      btnHover: '#ffffff26', btnDown: '#ffffff3d', borderHover: '#ffffff33',
      btnOnHover: '#f0b32352', renderOnHover: '#aeb4bc44',
      clear: 'rgba(0,0,0,0)',                      // NEVER style.X = '' (aborts the script)
      // Per-axis slider fill colors -- keep in sync with kChColors in GraphEditorExperimentHud.cpp
      // so the Framing sliders visually tie to the Graph editor's channel swatches/curves.
      // Order: Off X, Off Y, Off Z, Pitch, Yaw, Roll, FOV.
      //
      // Held at ONE chroma (0.145) across all seven hues, with lightness free to land where each
      // hue can reach that chroma. sRGB is not a cylinder -- at a fixed L, max chroma swings ~2.5x
      // across hue -- so L and C cannot both be constant. Chroma is the one to pin, because it is
      // what makes a channel look more or less emphatic: the old set drifted 0.124-0.181 chroma
      // AND 0.712-0.875 lightness, so Pitch-gold and Off-Y-green read as the important channels.
      // Now: chroma spread 0.001, lightness spread 0.076.
      //
      // Pitch moved off gold. It was #ffcf5a at H 86.6, and the workspace accent is H 82.4 -- one
      // data channel wearing the identity colour dilutes what gold signals everywhere else. It
      // now sits at H 342 (pink), the widest free gap, 39 deg from Yaw and 115 from Roll: the
      // rotation trio is the set most likely to be confused, so it gets the most separation.
      //
      // Off X stays RED even though dangerText is also a light red. X/Y/Z = red/green/blue is a
      // convention every 3D tool shares and it earns its keep here; the two are a slider FILL and
      // a button LABEL, never adjacent, and they now differ in lightness and chroma.
      axis: ['#ff8885cc', '#80d680cc', '#69a7ffcc', '#fd96d7cc', '#c89cffcc', '#25c8ffcc', '#ff9a5acc'],
      font: 'Stratum2, "Arial Unicode MS"',
      // Monospaced-DIGIT cut of the same family, shipped by CS2 and used by its own stylesheets for
      // scoreboard/timer figures. Use it for any readout whose NUMBER changes while you watch it:
      // proportional digits have different widths, so a centred value shifts on every update.
      fontMonodigit: 'Stratum2 Bold Monodigit, Stratum2, "Arial Unicode MS"'
    };
    // BOTTOM_LIFT 0 = the docked bottom editor is FLUSH with the bottom of the screen. It was 28,
    // to clear CS2's build/version string, which left a band of bare game render under every dock.
    // Keep in sync with GraphEditorJs BOTTOM_LIFT and CameraTimelineJs EDITOR_BOTTOM_LIFT.
    // 520 and a 22px gutter: Docs/panorama/panorama_ui_guide.md specifies ONE inspector column, and the Config
    // panel is already on it (UI.col in PanoramaUiMetricsJs.h). 430 could not hold two 76px
    // value tiles side by side, which is the unit the whole card grammar is built from.
    // A LITERAL, not UI.col.w: PanoramaUiMetricsJs.h is #include'd further down this file, so `UI`
    // does not exist yet at this point in the script. Keep the two in step by hand.
    var INSPECTOR_W = 640, BOTTOM_H = 176, BOTTOM_LIFT = 0;
    // The operator's dragged column width, published by C++ in st.inspW (InspectorWidth.h) and
    // shared with CONFIG and the docked timeline. INSPECTOR_W is only the value to use before the
    // first state arrives. The render already re-applies responsiveInspectorW() to the column,
    // the fades, the footer and the tab bar every frame, so changing this moves all of them.
    var edInspW = INSPECTOR_W;
    var edLayoutW = 0;   // the width the render last applied; rows re-fit when it changes
    function responsiveInspectorW(screenW) {
      var w = edInspW > 0 ? edInspW : INSPECTOR_W;
      // Same cap and floor as InspectorWidth.h, so the column, the preview and the docked bar
      // agree on where the edge is even when the stored width will not fit this screen.
      if (screenW > 1) w = Math.min(w, Math.max(600, screenW * 0.62));
      return Math.floor(Math.max(600, w));
    }
    // 42px: the tab buttons are ~30px tall (14px bold text + 12px padding) and the bar has 8px of
    // its own padding -- at the old 34px the button labels were clipped at the bottom.
    var TAB_BAR_H = 42, TAB_GAP = 4; // bottom-editor tab bar (DIVIDER above the active editor) height + gap
    var NATIVE_BAR_H = 96;  // fallback height for CS2's native demo bar (measured at runtime when shown)
    var TIMELINE_DOCK_H = 440; // the TIMELINE dock under the ribbon (keep in sync with GraphEditorJs DOCK_H)
    var CH_ROLL = 5, CH_FOV = 6;
    var dbgGeom = null; // [debug overlay] last preview geometry stashed by render() (px + fractions)
    var _lastInvariantSig = ''; // rate-limit layout-invariant warnings (one per unique violation)

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[editor] cmd failed: ' + e + '\n'); }
    }

)EDJS"
#include "PanoramaUiMetricsJs.h"
#include "InspectorLayoutJs.h"
#include "CameraEditorWidgetsJs.h"
// The original 2026-09 card grammar (cards, 76px tiles, action groups, sub-page tabs),
// shared with the Config panel. MUST follow the widget layer it builds on.
#include "CameraEditorCardsJs.h"
R"EDJS(
    // ---- root: full-screen, non-hittest. Children draw bottom -> top. ----
    var root = $.CreatePanel('Panel', ctx, 'CamEditorRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '53'; // z-layer map: PanoramaBridge.h

    // TOP-LEVEL root (sibling of CamEditorRoot, NOT a child): a child z-index is local to its
    // parent, so as a z200 child of the z53 editor root this modal rendered BEHIND the Graph
    // editor root (z65) and the timeline root (z55). Own root at z220 wins over every HUD root.
    // Deleted alongside CamEditorRoot in CameraEditorHud::Teardown.
    var confirmOverlay = $.CreatePanel('Panel', ctx, 'CamEditorConfirmRoot', {});
    confirmOverlay.visible = false; confirmOverlay.hittest = true;
    confirmOverlay.style.width = '100%'; confirmOverlay.style.height = '100%'; confirmOverlay.style.zIndex = '220';
    confirmOverlay.style.backgroundColor = 'rgba(0,0,0,0.72)';
    confirmOverlay.SetPanelEvent('onactivate', function () { hideClearConfirm(); });
    var confirmBox = mk('Panel', confirmOverlay); confirmBox.hittest = true;
    confirmBox.style.width = '520px'; confirmBox.style.height = '230px';
    confirmBox.style.horizontalAlign = 'center'; confirmBox.style.verticalAlign = 'center';
    confirmBox.style.backgroundColor = 'rgba(16,20,26,0.99)';
    confirmBox.style.border = '1px solid ' + S.accent; confirmBox.style.borderRadius = '6px';
    confirmBox.style.boxShadow = '#000000ee 0px 0px 28px 6px';
    confirmBox.style.flowChildren = 'down';
    confirmBox.style.paddingTop = '26px'; confirmBox.style.paddingBottom = '24px';
    confirmBox.style.paddingLeft = '28px'; confirmBox.style.paddingRight = '28px';
    confirmBox.SetPanelEvent('onactivate', function () { /* keep clicks inside from dismissing */ });
    var confirmTitle = lbl(confirmBox, 'CLEAR ALL CAMERA PATHS?', S.value, 22);
    confirmTitle.style.fontWeight = 'bold'; confirmTitle.style.letterSpacing = '2px';
    confirmTitle.style.horizontalAlign = 'center'; confirmTitle.style.marginBottom = '14px';
    var confirmBody = lbl(confirmBox, 'This deletes every path camera/keyframe for this demo.', S.label, 14);
    confirmBody.style.horizontalAlign = 'center'; confirmBody.style.marginBottom = '22px';
    var confirmActions = mk('Panel', confirmBox); confirmActions.style.flowChildren = 'right';
    confirmActions.style.horizontalAlign = 'center';
    function hideClearConfirm() { confirmOverlay.visible = false; }
    var confirmAction = null;
    function showClearConfirm() {
      confirmTitle.text = 'CLEAR ALL CAMERA PATHS?';
      confirmBody.text = 'This deletes every path camera/keyframe for this demo.';
      // Restated because SCENE KEYS borrows this dialog and rewrites the verb (skConfirm).
      confirmYes.__lbl.text = 'Yes, clear all';
      confirmAction = null; confirmOverlay.visible = true;
    }
    var confirmNo = btn(confirmActions, 'No', function () { hideClearConfirm(); }, S.value);
    confirmNo.style.width = '132px'; confirmNo.__lbl.style.textAlign = 'center'; confirmNo.__lbl.style.width = '100%';
    var confirmYes = btnDanger(confirmActions, 'Yes, clear all', function () {
      hideClearConfirm();
      if (confirmAction) { var action = confirmAction; confirmAction = null; action(); }
      else cmd('mirv_filmmaker camtl clear');
    });
    confirmYes.style.width = '172px'; confirmYes.style.marginRight = '0px';
    confirmYes.__lbl.style.textAlign = 'center'; confirmYes.__lbl.style.width = '100%';

    // Preview catcher FRAME: four panels ringing the preview rect (top/bottom/left/right).
    // Empty-space clicks land on the frame and are swallowed -- they must NOT switch spectator
    // targets. Clicks INSIDE the preview hit nothing of ours and pass through to the game,
    // where CS2's native LMB/RMB spectator switching works. Inspector/backdrop children are
    // drawn AFTER these, so their own clicks still land. UI-cursor mode only (hittest is set
    // per-frame in render()).
    // THE WORKSPACE SHELL (WorkspaceShellJs.h) -- the same catchers, tag, masks, frame and
    // decor the CONFIG panel uses; only the accent and the wordmark differ. The editor's tag
    // dot pulses like a recording light; the shell owns that too.
    var shell = mvmWorkspaceShell(root, {
      accent: S.accent, title: 'CAMERA / EDITOR', code: 'SOURCE // MVM FILMMAKER',
      onCatcher: function () { closeAllDrops(); }
    });
    shell.pulse(true);
    var catcherTop = shell.catchers.top, catcherBottom = shell.catchers.bottom;
    var catcherLeft = shell.catchers.left, catcherRight = shell.catchers.right;
    var tag = shell.tag, tagLbl = shell.tagLbl;

    // Bottom backdrop (sits BEHIND the CameraTimeline panel, which is z55) = workspace
    // footer under the preview.
    var backdrop = shell.backdrop;
    backdrop.style.position = '0px 0px 0px';
    backdrop.style.width = '100%'; backdrop.style.height = BOTTOM_H + 'px';

    // Bottom-editor tab bar: a DIVIDER sitting directly ABOVE the active bottom editor with the
    // three bottom-panel switches (where CS2's old CAM EDITOR / MOUSE buttons sat). It stays put
    // across modes; render() positions it just above whichever editor is docked below it, and sizes
    // its width. The native demo bar's own buttons are suppressed while the editor is open, so these
    // tabs replace them. Pure switches (NOT toggles): always select that mode -> never an empty bottom.
    var curBottomMode = 'native';
    // THE SHARED DOCK RIBBON -- one definition for this workspace and CONFIG, see
    // TimelineDockRibbonJs.h. It replaces this panel's own three-tab bar, its `bottomTab`
    // builder and its own gear, all of which existed in a second copy in ConfigHudJs.h.
    var dockRibbon = mvmTimelineDockRibbon(root, S.accent, function () { toggleSettings(); });
    var tabBar = dockRibbon.root;
    tabBar.id = 'EdTabBar';
    tabBar.style.position = '0px 0px 0px';
    var gearBtn = dockRibbon.gear;
)EDJS"
R"EDJS(
    // ===================== EDITOR SETTINGS MENU =====================
    // Small fly-out over the bottom-right that mirrors CS2's native demo-playback settings
    // (huddemocontroller.xml). Each toggle drives the SAME cvar the native panel does, read
    // back live so the pill reflects the engine state:
    //   X-Ray              -> spec_show_xray 0|1
    //   True View          -> cl_demo_predict 0|1  (2 = also allow wrong demo version)
    //   Include DOA actions -> cl_trueview_show_doa_predictions 0|1   (needs True View on)
    //   Allow demo mismatch -> cl_demo_predict 1<->2                  (needs True View on)
    function rdInt(name) {
      try { var v = parseInt(GameInterfaceAPI.GetSettingString(name)); return isFinite(v) ? v : 0; }
      catch (e) { return 0; }
    }
)EDJS"
#include "WorkspaceShellJs.h"
#include "WorkspaceChromeJs.h"
#include "TimelineDockRibbonJs.h"
#include "InspectorResizeJs.h"
#include "EditorSettingsPopupJs.h"
R"EDJS(
    // ---- EDITOR SETTINGS fly-out (shared with the Config panel) --------------------------
    // The card itself lives in EditorSettingsPopupJs.h so the Camera Editor and Config show the
    // SAME popup, and a fifth toggle can never land on only one of them. This adapter keeps the
    // editor's existing names -- settingsOverlay / settingsCard / openSettings / closeSettings /
    // toggleSettings / refreshSettings -- so every existing call site (the per-frame closers, the
    // render-workspace close, the marginBottom reposition, and the api.* debug hooks) is
    // unchanged. The overlay is still 'CamEditorSettingsRoot', which CameraEditorHud::Teardown
    // deletes by id.
    var settingsPopup = mvmEditorSettingsPopup(ctx, {
      id: 'CamEditorSettingsRoot',
      publish: root,
      marginBottom: BOTTOM_H + 16,
      accent: S.freeze,          // Editor Settings owns the theme BLUE, not the editor's gold
      debugTag: 'camsettings',
      debugOn: function () { return mvmDebugOn(); },
      debugState: function () { return 'en=' + (st && st.enabled) + ' cur=' + (st && st.cursor); },
      beforeOpen: function () { closeAllDrops(); },
      onOpen: function () { gearBtn.style.backgroundColor = S.accent.substring(0, 7) + '33'; },
      onClose: function () { gearBtn.style.backgroundColor = '#ffffff14'; }
    });
    var settingsOverlay = settingsPopup.overlay;
    var settingsCard = settingsPopup.card;
    function refreshSettings() { settingsPopup.refresh(); }
    function openSettings() { settingsPopup.open(); }
    function closeSettings(why) { settingsPopup.close(why); }
    function toggleSettings() { if (settingsOverlay.visible) closeSettings('gearReclick'); else openSettings(); }

    // Shared action row: fixed height and real flow layout keep both actions aligned without
    // hard-coded right offsets. Widths intentionally differ; spacing and baseline do not.
    // RENDER / CUSTOMIZE PLAYER from the one shared builder (WorkspaceChromeJs.h), with
    // CONFIG's geometry: the row spans the column and CUSTOMIZE PLAYER fills what RENDER leaves.
    // The CUSTOMIZE button used to be built 2000 lines away in CameraEditorCustomizeJs.h at a
    // hard 208px; openCustomize() is a hoisted declaration there, so it is reachable from here.
    var dockActions = mvmActionRow(root, interactive, [
      { text: 'RENDER', width: 150, onClick: function () { openRender(); } },
      { text: '\u2726  CUSTOMIZE PLAYER', width: 'fill-parent-flow(1.0)',
        onClick: function () { openCustomize(); }, press: true }
    ]);
    var actionRow = dockActions.root;
    var renderBtnEl = dockActions.buttons[0], renderBtnLbl = renderBtnEl.__lbl;
    mvmTourAdd('editor.render', renderBtnEl);
    var customizeBtn = dockActions.buttons[1];

)EDJS"
#include "CameraEditorCustomizeJs.h"
R"EDJS(
    // The column's ground: the Render workspace's background (WorkspaceShellJs.h mvmColumnGround),
    // tinted gold. Root-level, full height, built BEFORE the inspector so it paints under it; the
    // inspector and the footer band behind RENDER / CUSTOMIZE PLAYER are transparent over it.
    var columnGround = mvmColumnGround(root, S.accent, INSPECTOR_W);
    // Right inspector column.
    var inspector = mk('Panel', root); inspector.hittest = false;
    inspector.style.horizontalAlign = 'right'; inspector.style.verticalAlign = 'top';
    inspector.style.width = INSPECTOR_W + 'px'; inspector.style.height = '100%';
    inspector.style.backgroundColor = S.clear; inspector.style.borderLeft = '1px solid ' + S.clear;
    inspector.style.flowChildren = 'down';
    inspector.style.paddingTop = UI.col.pad + 'px';
    // Just breathing room under the last card. Clearing the floating RENDER / CUSTOMIZE PLAYER row
    // is NOT this padding's job -- render() shortens the inspector's HEIGHT to end above it, which
    // is the only thing that keeps mid-scroll content out from under those buttons. See the note
    // at that assignment.
    inspector.style.paddingBottom = '10px';
    var lastInspectorH = -1;
    inspector.style.paddingLeft = UI.col.pad + 'px'; inspector.style.paddingRight = UI.col.pad + 'px';
    inspector.style.fontFamily = S.font;
    // CLIP, not native scroll. Panorama never receives the mouse wheel in this in-game HUD context
    // -- MovieMode::OnMouseWheel consumes it so it cannot reach the demo spectator behind the
    // panel -- so `overflow: scroll` drew a scrollbar NOTHING could drive: pointing at the middle
    // of the inspector and scrolling did nothing, while the Config panel (which already scrolls by
    // hand) worked. The column is scrolled manually instead: C++ forwards each captured notch to
    // api.wheel() and edApplyScroll() translates `scrollWrap`. Same approach, and the same reason,
    // as ConfigHudJs.h and the Customize modal's control column.
    // CLIP, NEVER SQUISH. `squish` lets Panorama SHRINK flow children when the column's content
    // overflows it, and a real mouse-wheel event runs through Panorama's own layout as well as
    // ours: on that pass the scrolled subtree still counts its full height, the column overflows,
    // and the pinned chrome gets squished -- Labels have no explicit height, so they go to zero
    // and the header chips and tab captions vanish while their bordered boxes survive. That is
    // the "text disappears when I scroll" bug. Nothing here ever needs squishing: scrollView is
    // fill-parent-flow and takes exactly what is left, so plain clip is always correct.
    inspector.style.overflow = 'clip';

    // Bottom clearance for the floating RENDER / CUSTOMIZE PLAYER row, and the fade that dissolves
    // content into it instead of slicing it off. Both mirror the Config panel exactly -- one
    // behaviour, two workspaces.
    var ACTION_CLEAR = UI.action.h + UI.action.inset * 2;
    var FADE_H = 46;
    var ED_OVERSCROLL = 18;   // a little travel past "the last control has cleared the fade"

    // Declared here because the scroll helpers below are defined before the panels exist (var
    // hoisting); they are only ever CALLED after the build has finished.
    var scrollView, scrollWrap;

    // Scrollbar thumb -- the only affordance that the column HAS more content, now that the native
    // (undrivable) one is gone. Parented to `root`, not the inspector, so it never scrolls itself.
    var scrollThumb = mk('Panel', root); scrollThumb.hittest = false; scrollThumb.visible = false;
    scrollThumb.style.horizontalAlign = 'right'; scrollThumb.style.verticalAlign = 'top';
    scrollThumb.style.marginRight = '3px'; scrollThumb.style.width = '4px';
    scrollThumb.style.borderRadius = '2px';
    scrollThumb.style.backgroundColor = '#ffffff33';
    scrollThumb.style.zIndex = '105';

    // Opaque band behind the floating action row, so scrolled content genuinely ends above the
    // buttons instead of showing through the gaps between them.
    var footerBg = mk('Panel', root); footerBg.hittest = false; footerBg.visible = false;
    footerBg.style.horizontalAlign = 'right'; footerBg.style.verticalAlign = 'bottom';
    footerBg.style.width = INSPECTOR_W + 'px'; footerBg.style.height = ACTION_CLEAR + 'px';
    // Transparent over the column ground: the inspector already ends above the buttons, so this
    // band only has to exist for its geometry, and a fill here was the flat strip under the buttons.
    footerBg.style.backgroundColor = S.clear;
    footerBg.style.borderLeft = '1px solid ' + S.clear;
    footerBg.style.zIndex = '100';

    // The cut-off fade: content dissolves into the footer instead of being sliced off by it. This
    // is the same gradient the Config panel uses -- the editor had a hard edge where Config had a
    // fade, which is the difference you can see when you scroll either one.
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

    // ---- the column's drag edge, same control as CONFIG's (InspectorResizeJs.h) -------------
    // The render re-applies responsiveInspectorW() to the column, the fades, the footer, the tab
    // bar and the scaled preview every frame, so the grip only has to move the value.
    var edGripBusy = false;
    var edGrip = mvmInspectorGrip({
      root: root,
      colour: S.accent,
      screenW: function () {
        var sx = ctx.actualuiscale_x || root.actualuiscale_x || 1;
        var raw = ctx.actuallayoutwidth || root.actuallayoutwidth || 0;
        return raw / sx;
      },
      apply: function (w) {
        edInspW = w;
        // Same direct hand-off CONFIG makes: the docked timeline / camera / graph are other
        // panels and would otherwise only learn the width when the drag is released.
        mvmPublishInspW(w);
      },
      commit: function (w) { cmd('mirv_filmmaker settings ui inspectorW ' + w); }
    });

    // ---- manual scrolling ----------------------------------------------------------------
    var edScrollY = 0, edAppliedScrollY = -1;
    function edViewH() {
      var vsy = inspector.actualuiscale_y || 1;
      return ((scrollView && scrollView.actuallayoutheight) || 0) / vsy;
    }
    function edScrollTop() {
      try {
        var vsy = root.actualuiscale_y || 1;
        var p = scrollView.GetPositionWithinWindow();
        return p && isFinite(p.y) ? p.y / vsy : 0;
      } catch (e) { return 0; }
    }
    function edContentH() {
      var vsy = inspector.actualuiscale_y || 1;
      return ((scrollWrap && scrollWrap.actuallayoutheight) || 0) / vsy;
    }
    function edApplyScroll() {
      if (!scrollWrap || !scrollView) return;
      var viewH = edViewH(), contH = edContentH();
      // What you can actually READ is the viewport minus the bottom fade -- content under the fade
      // has already dissolved into the footer -- so that is what the content has to clear. maxY is
      // ZERO when the content already fits: a page that fits must not scroll at all, or one notch
      // slides the top card up under the pinned tab strip for no reason.
      var usableH = Math.max(0, viewH - FADE_H);
      var maxY = contH > usableH ? (contH - usableH + ED_OVERSCROLL) : 0;
      if (edScrollY > maxY) edScrollY = maxY;
      if (edScrollY < 0) edScrollY = 0;
      // POSITION, NOT TRANSFORM. A transform puts the scrolling subtree on its own compositing
      // layer, and this column owns clipped content AND pinned text -- which is what made the
      // header and tab strip flicker and drop characters while scrolling. Writing `position`
      // moves the same pixels with no layer promotion. Guarded on a real change because
      // render() runs every frame and a repeated style write still costs a layout pass.
      var edScrollPx = Math.round(edScrollY);
      if (edScrollPx !== edAppliedScrollY) {
        edAppliedScrollY = edScrollPx;
        // marginTop, matching the Config panel. Which property moves the column turned out not
        // to matter: this inspector had the same chrome dropout, and the fix for both was the
        // zIndex on `head`/`modeRow`, not the choice of scroll property.
        scrollWrap.style.marginTop = (-edScrollPx) + 'px';
      }
      if (!scrollThumb) return;
      if (maxY <= 0.5 || viewH <= 0 || contH <= 0) { scrollThumb.visible = false; return; }
      var th = Math.max(30, viewH * (viewH / contH));
      var ty = (viewH - th) * (edScrollY / maxY);
      scrollThumb.visible = true;
      scrollThumb.style.height = Math.round(th) + 'px';
      scrollThumb.style.marginTop = Math.round(edScrollTop() + ty) + 'px';
    }
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent literals are
// concatenated by the compiler, so this stays one JS blob.
R"EDJS(

    // Attention pulse for the inspector (fired by "editor flashinspector", i.e. F on an aimed-at
    // camera). An overlay rather than restyling the inspector itself: ignoreParentFlow keeps it
    // out of the scroll column so it can never move a control, and fading opacity is the one
    // property that cannot disturb layout.
    var flashOverlay = mk('Panel', inspector); flashOverlay.hittest = false;
    flashOverlay.style.ignoreParentFlow = 'true';
    flashOverlay.style.width = '100%'; flashOverlay.style.height = '100%';
    flashOverlay.style.position = '0px 0px 0px';
    flashOverlay.style.backgroundColor = S.accent;
    flashOverlay.style.opacity = '0';
    flashOverlay.style.transitionProperty = 'opacity';
    flashOverlay.style.transitionDuration = '0.42s';
    flashOverlay.style.transitionTimingFunction = 'ease-out';
    var flashSeen = -1;
    function pulseInspector() {
      flashOverlay.style.transitionDuration = '0.05s';
      flashOverlay.style.opacity = '0.16';
      try {
        $.Schedule(0.06, function () {
          flashOverlay.style.transitionDuration = '0.42s';
          flashOverlay.style.opacity = '0';
        });
      } catch (e) { flashOverlay.style.opacity = '0'; }
    }

    // Aspect-ratio letterbox: the live preview is sized to the GAME's aspect ratio
    // (rootW/rootH) and CENTRED in the available area (screen minus the right inspector and
    // the bottom bar); the leftover is masked with black bars on all four sides and an accent
    // frame outlines the rect. Geometry is computed each render from the laid-out root size.
    // (The game still renders full-screen behind, so the preview is a CROP shaped to the
    // render aspect when the scaler is off, or a true scaled blit when it is on.)
    var barLeft = shell.masks.left, barRight = shell.masks.right;
    var barTop = shell.masks.top, barBottom = shell.masks.bottom;
    // frameV / frameH were this panel's names for the right and bottom edges.
    var frameL = shell.frames.left, frameV = shell.frames.right;
    var frameT = shell.frames.top, frameH = shell.frames.bottom;
    var workspaceDecor = shell.decor;

    root.SetAttributeString('debugpanels', '[]');
    function hidePreviewLayout() {
      frameL.visible = frameV.visible = frameT.visible = frameH.visible = false;
      barLeft.visible = barRight.visible = barTop.visible = barBottom.visible = false;
      workspaceDecor.visible = false;
      catcherTop.visible = catcherBottom.visible = catcherLeft.visible = catcherRight.visible = false;
      tag.visible = false;
      root.SetAttributeString('previewrect', '');
      root.SetAttributeString('debugpanels', '[]');
      dbgGeom = null;
    }

    // ===================== HEADER =======================================
    // ONE row: the workspace name, then its chips (Docs/panorama/panorama_ui_guide.md, Shared workspace contract). The chips
    // are bordered text, not filled buttons -- a filled button in the chrome competes with the
    // card actions below it, and the original design's header is deliberately quiet.
    // PINNED CHROME GETS ITS OWN STACKING CONTEXT. Without an explicit zIndex this row is
    // composited together with the clipped scroll column beside it, and while a card straddles
    // the viewport's top edge the engine drops the row's children from the frame -- the boxes
    // keep their exact geometry and the text simply stops painting. See the header of
    // scratchpad patch_chromelayer / the note in cfgApplyScroll for the measurements. Purely a
    // paint-order change: this row sits above the scroll viewport in flow and never overlaps it.
    // EXACTLY the Config panel's head (ConfigHudJs.h): a plain right-flowing row. row() also
    // applies `vertical-align: middle`, which made this row 6px taller than Config's -- and since
    // both tab strips hang off it with the same 20px marginTop, that 6px pushed the editor's tab
    // bar, and every card under it, 6px below where Config puts them. Same chrome, same place.
    var head = mk('Panel', inspector); head.hittest = false; head.id = 'EdHead';
    head.style.flowChildren = 'right'; head.style.width = '100%';
    head.style.zIndex = '80';
    var hTitle = lbl(head, 'CAMERA EDITOR', S.value, 18); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2.5px'; hTitle.style.width = 'fill-parent-flow(1.0)';
    hTitle.style.verticalAlign = 'center'; hTitle.style.whiteSpace = 'nowrap';
    // Same chip as the CONFIG panel's, from the same definition -- see WorkspaceChromeJs.h.
    function edChip(text, onClick) {
      return mvmChromeChip(head, function (parent, t, f) { return btn(parent, t, f, S.label); },
        btnVisual, text, onClick);
    }
    // No MOUSE: UI/GAME chip: where you click decides it (Movie/UiClickMath.h). The preview is
    // GAME, everything else is UI.
    // Exit fully closes the editor and restores the native CS2 demo bar. The bottom-panel switch
    // (Camera Timeline / Graph) lives in the persistent bottom tab bar, not here.
    var exitBtn = edChip('ESC EXIT', function () { cmd('mirv_filmmaker editor close'); });
    exitBtn.id = 'EdExit';
    // (`mirv_filmmaker editor hud hidden|game|full` drives the Game-UI visibility cycle.)

    var inspectorMode = 'path';
    var renderWsSeen = false;    // drives the workspace-arrival entrance
    var inspectorTabSeen = '';   // last mode an entrance was played for (see the swap guard)
    var editorUiHydrated = false;
    // Inspector tab strip: TEXT TABS ON A RULE, uppercase and letterspaced, the active one
    // underlined in the workspace accent -- the same grammar the Config panel uses, so the two
    // surfaces read as one product (Docs/panorama/panorama_ui_guide.md).
    var modeRow = mk('Panel', inspector); modeRow.style.flowChildren = 'right'; modeRow.style.width = '100%';
    modeRow.style.zIndex = '80';   // same reason as `head` above
    modeRow.style.marginTop = '20px'; modeRow.style.borderBottom = '1px solid #ffffff12';
    function edWorkspaceTab(caption, onClick, last) {
      var b = tabify(btn(modeRow, caption, onClick, S.label));
      // Sized to its own caption with a fixed gap, not stretched: two stretched tabs put
      // FOLLOW CAMERA's 13 characters in the same slot as PATH CAMERA's 11.
      // EXACTLY the Config panel's top tab strip (ConfigHudJs.h): fit-children, UI.tabTop gap,
      // font and letter-spacing. This is the same control in the same place in both workspaces, and
      // a third tab is not a reason to shrink it -- when SCENE KEYS was added these were dropped to
      // 12px/0.5 and stretched to equal widths, so the editor's navigation stopped matching Config's.
      b.style.width = 'fit-children';
      b.style.paddingLeft = '0px'; b.style.paddingRight = '0px';
      // paddingTop is RESTATED, not inherited: this workspace's tabify() uses 8px (that is right
      // for the in-card sub-page tabs it also builds) while Config's uses 2px. Six pixels of top
      // padding is the whole reason the editor's tab strip -- and every card hanging under it --
      // sat 6px lower than Config's, with identical heads above and identical margins below.
      b.style.paddingTop = '2px';
      b.style.paddingBottom = UI.tabTop.padBottom + 'px';
      b.style.marginRight = last ? '0px' : UI.tabTop.gap + 'px';
      b.__lbl.style.fontSize = UI.tabTop.font + 'px';
      b.__lbl.style.letterSpacing = UI.tabTop.ls + 'px';
      return b;
    }
    var pathTab = edWorkspaceTab('PATH CAMERA', function () {
      inspectorMode = 'path';
      cmd('mirv_filmmaker settings ui editor.inspector path');
    });
    var followTab = edWorkspaceTab('FOLLOW CAMERA', function () {
      inspectorMode = 'follow';
      cmd('mirv_filmmaker settings ui editor.inspector follow');
    });
    var sceneTab = edWorkspaceTab('SCENE KEYS', function () {
      inspectorMode = 'scene';
      cmd('mirv_filmmaker settings ui editor.inspector scene');
    }, true);
    mvmTourAdd('editor.tab.path', pathTab); mvmTourAdd('editor.tab.follow', followTab); mvmTourAdd('editor.tab.scene', sceneTab);
    // FOLLOW's pinned status bar (Docs/features/camera-editor-feature.md): the target line, the state, and Live / Stop.
    // It sits ABOVE the rail so Live works from any Follow card. Built here, filled by
    // CameraEditorFollowInspectorJs.h; shown only on the Follow tab.
    var edFollowBar = mk('Panel', inspector); edFollowBar.hittest = false; edFollowBar.hittestchildren = true;
    edFollowBar.style.flowChildren = 'right'; edFollowBar.style.width = '100%';
    edFollowBar.style.marginTop = '14px'; edFollowBar.style.height = '44px';
    edFollowBar.style.paddingLeft = '12px'; edFollowBar.style.paddingRight = '6px';
    edFollowBar.style.borderRadius = '3px';
    edFollowBar.style.backgroundColor = '#11151bff'; edFollowBar.style.border = '1px solid #ffffff14';
    edFollowBar.visible = false;

    // ---- the body: the rail on the left, the scrolling pane on the right ----------------------
    // Everything above (title + Exit, the tab strip) is PINNED. Only the pane scrolls; the rail
    // holds one row per card and never needs to.
    //
    // `inspectorBody` is what the card fragments parent to; the rail selection decides which of
    // their cards is visible.
    var edBodyRow = mk('Panel', inspector); edBodyRow.hittest = false; edBodyRow.hittestchildren = true;
    edBodyRow.style.flowChildren = 'right'; edBodyRow.style.width = '100%';
    edBodyRow.style.height = 'fill-parent-flow(1.0)'; edBodyRow.style.marginTop = '14px';
    var edRailHost = mk('Panel', edBodyRow); edRailHost.hittest = false; edRailHost.hittestchildren = true;
    edRailHost.style.flowChildren = 'down';
    edRailHost.style.width = UI.rail.w + 'px'; edRailHost.style.height = '100%';
    edRailHost.style.paddingRight = UI.rail.padR + 'px';
    edRailHost.style.borderRight = '1px solid #ffffff12';
    edRailHost.style.overflow = 'clip';
    // The rail SCROLLS ON ITS OWN when its rows outgrow the column: the Path rail carries one row
    // per camera plus the add button, and a long path put the later cameras under the clip with
    // no way to reach them. A wheel notch while the pointer is over the rail moves the active
    // tab's rows (edRailWheel, from api.wheel); over the pane it scrolls the pane as before. The
    // pointer is known from the host's own hover (it bubbles up from the rows) or any hot row.
    var edRailHot = false, edRailScrollY = 0, edRailAppliedY = -1;
    edRailHost.hittest = true;
    edRailHost.SetPanelEvent('onmouseover', function () { edRailHot = true; });
    edRailHost.SetPanelEvent('onmouseout', function () { edRailHot = false; });
    function edRailPointerOver() {
      if (edRailHot) return true;
      var r = edRails[edActiveTab];
      if (!r) return false;
      for (var k in r.items) { var it = r.items[k]; if (it.row && it.row.__hot) return true; }
      return false;
    }
    function edRailApplyScroll() {
      var r = edRails[edActiveTab];
      if (!r || !r.host) return;
      var sy = inspector.actualuiscale_y || 1;
      var viewH = (edRailHost.actuallayoutheight || 0) / sy, contH = (r.host.actuallayoutheight || 0) / sy;
      var maxY = contH > viewH ? (contH - viewH + 8) : 0;
      if (edRailScrollY > maxY) edRailScrollY = maxY;
      if (edRailScrollY < 0) edRailScrollY = 0;
      var px = Math.round(edRailScrollY);
      if (px !== edRailAppliedY) { edRailAppliedY = px; r.host.style.marginTop = (-px) + 'px'; }
    }
    // Takes the notch when the pointer is over a rail that overflows; false hands it to the pane.
    function edRailWheel(n) {
      var r = edRails[edActiveTab];
      if (!r || !r.host || !edRailPointerOver()) return false;
      var sy = inspector.actualuiscale_y || 1;
      if (((r.host.actuallayoutheight || 0) / sy) <= ((edRailHost.actuallayoutheight || 0) / sy)) return false;
      edRailScrollY -= n * 90;
      edRailApplyScroll();
      return true;
    }
    scrollView = mk('Panel', edBodyRow);
    scrollView.style.width = 'fill-parent-flow(1.0)';
    scrollView.style.height = '100%';
    scrollView.style.marginLeft = UI.rail.gap + 'px';
    scrollView.style.overflow = 'clip';   // clip only: its child is positioned, never squished
    scrollWrap = mk('Panel', scrollView);
    scrollWrap.style.flowChildren = 'down';
    scrollWrap.style.width = '100%';
    scrollWrap.style.height = 'fit-children';
    scrollWrap.style.transitionDuration = '0s';   // never animate the scrolled subtree
    var inspectorBody = scrollWrap;

    // ---- the key strip (PATH tab) -----------------------------------------------------------
    // The shortcuts, pinned under the body above RENDER (2026-10 sidebar cleanup), in the order
    // the work happens: K place, J arm the path, then the rest. Checked against MovieMode.cpp:
    // arrows skip kSkipSeconds (5), Shift+wheel is speed, Ctrl+wheel is FOV, X / Z roll.
    // Sized to be read at a glance from the game: 13px, key caps with real padding, the words in
    // the label tier. (The first cut was 11px and muted, and could barely be seen over the map.)
    var edKeyStrip = mk('Panel', inspector); edKeyStrip.hittest = false;
    edKeyStrip.style.flowChildren = 'right-wrap'; edKeyStrip.style.width = '100%';
    edKeyStrip.style.marginTop = '8px'; edKeyStrip.style.paddingTop = '10px'; edKeyStrip.style.paddingBottom = '2px';
    edKeyStrip.style.borderTop = '1px solid #ffffff14';
    [['K', 'place'], ['J', 'arm path'], ['L', 'delete'], ['F', 'edit'], ['Space', 'play'], ['← →', '±5 s'],
     ['Shift+Scroll', 'move speed'], ['Ctrl+Scroll', 'FOV'], ['X / Z', 'roll right / left']].forEach(function (d) {
      var item = mk('Panel', edKeyStrip); item.hittest = false;
      item.style.flowChildren = 'right'; item.style.marginRight = '16px'; item.style.marginBottom = '7px';
      var k = mk('Panel', item); k.hittest = false;
      k.style.paddingLeft = '8px'; k.style.paddingRight = '8px'; k.style.paddingTop = '3px'; k.style.paddingBottom = '3px';
      k.style.border = '1px solid #ffffff40'; k.style.borderRadius = '3px'; k.style.marginRight = '7px';
      k.style.backgroundColor = '#ffffff10';
      k.style.verticalAlign = 'center';
      var kl = lbl(k, d[0], S.value, 13); kl.hittest = false; kl.style.fontWeight = 'bold'; kl.style.whiteSpace = 'nowrap';
      var w = lbl(item, d[1], S.label, 13); w.hittest = false; w.style.verticalAlign = 'center'; w.style.whiteSpace = 'nowrap';
    });
    edKeyStrip.visible = false;

)EDJS"
#include "CameraEditorFollowInspectorJs.h"
#include "CameraEditorPathInspectorJs.h"
#include "CameraEditorSceneKeysJs.h"
#include "CameraEditorDebugJs.h"
#include "CameraEditorRenderJs.h"
R"EDJS(
    // =====================================================================
    // The native CS2 demo controller's "End Playback" button and its own gear (X-Ray/True View
    // panel, replaced by our ⚙ menu) are hidden PERMANENTLY now -- not just while this editor is
    // open -- see CameraTimelineJs.h patchNativeBar(), which runs every frame from demo start
    // regardless of editor state (this workspace doesn't even exist until first opened).

    var st = null;
    var api = {};
    api.setInspectorMode = function (mode) {
      inspectorMode = (mode === 'follow') ? 'follow' : ((mode === 'scene') ? 'scene' : 'path');
    };
    api.setAttachmentMenuOpen = function (open) {
      // Attach pt is a native DropDown now; toggling it open programmatically isn't needed.
    };
    // Live-tweak hooks for the 3D preview so the panel incantation / model paths can be dialed in
    // over netcon without rebuilding, e.g.:
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.previewInfo()+String.fromCharCode(10));
    //   mirv_filmmaker editor eval $.CamEditor.previewModel('agents/models/ctm_fbi/ctm_fbi.vmdl');
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.previewCall('SetActiveCharacter',5)+'\n');
    api.previewModel = function (p) { ensurePreview3d(p || teamDefaultModel()); applyPreview(); return previewState(); };
    api.previewItem = function (defIndex, paintKit) { var p = ensureViewmodelPreview(); setItemPreviewItem(p, makeFauxItemId(defIndex, paintKit || 0)); return previewState(); };
    api.previewCall = function (m, a) { if (preview3d && typeof preview3d[m] === 'function') { try { return '' + preview3d[m](a); } catch (e) { return 'ERR ' + e; } } return 'no3d'; };
    api.previewItemCall = function (m, a) { if (previewItem3d && typeof previewItem3d[m] === 'function') { try { return '' + previewItem3d[m](a); } catch (e) { return 'ERR ' + e; } } return 'noitem'; };
    api.openCustomize = function () { openCustomize(); return previewState(); };
    api.closeCustomize = function () { closeCustomize(); return 'closed'; };
    api.previewPick = function (slot, value) { pickCosmetic(slot, '' + value); return previewState(); };
    api.customizePick = function (slot, value) { pickCosmetic(slot, '' + value); return JSON.stringify({ sel: custSel, wear: custWear, preview: previewState() }); };
    api.customizeWear = function (slot, preset, value) { setWear(slot, preset || 'fn', value === undefined ? null : parseFloat(value)); return JSON.stringify({ sel: custSel, wear: custWear, preview: previewState() }); };
    api.customizeOpenPicker = function (slot) { slot = ('' + (slot || '')).replace(/^\s+|\s+$/g, ''); populateCustomize(); var d = customizeDrops[slot]; if (d && d.open) d.open(); return slot || ''; };
    api.customizeOpenWear = function (slot) { slot = ('' + (slot || '')).replace(/^\s+|\s+$/g, ''); populateCustomize(); var d = customizeDrops[slot]; if (d && d.open) d.open(); return slot || ''; };
    api.customizeSearch = function (slot, query) { slot = ('' + (slot || '')).replace(/^\s+|\s+$/g, ''); populateCustomize(); var d = customizeDrops[slot]; if (d && d.setSearch) { d.setSearch(query || ''); d.open(); } return query || ''; };
    // Captured-input pipe from CameraEditorHud (WndProc-thread wheel + WM_CHAR, forwarded on the main
    // thread). custWheel/custChars are hoisted function declarations in CameraEditorCustomizeJs.h.
    api.custWheel = function (n, x, y) { return custWheel(n, x, y); };
    api.wordmark = function (at) { shell.replay(at); return 'wordmark ' + (at === undefined ? 'replay' : at); };
    // Two consumers now share this one character queue: the Customize modal's search boxes and the
    // Render workspace's typed number fields. A focused render field wins, because it is the more
    // specific target and the two surfaces are never usefully typed into at the same time.
    api.custChars = function (csv) {
      if (csv == null) return 'closed';
      var parts = ('' + csv).split(',');
      if (renderFocusField) {
        for (var i = 0; i < parts.length; i++) {
          var code = parseInt(parts[i], 10);
          if (code >= 0) renderFieldChar(code);
        }
        return 'render';
      }
      return custChars(csv);
    };
    // Focus a Render workspace typed field from the console
    // (mirv_filmmaker editor eval $.CamEditor.renderFocus('fps')), so the WM_CHAR -> custChars ->
    // field path can be exercised without a mouse. Returns the published textcapture flag.
    api.renderFocus = function (which) {
      renderFocusFieldByName(which);
      return (which || '') + ' textcapture=' + root.GetAttributeString('textcapture', '?');
    };
    // Render workspace interactivity probe, driven from the console with no quotes (the netcon
    // arg parser eats them, so an inline JS string literal never survives the trip):
    //   mirv_filmmaker editor eval $.CamEditor.renderProbe()
    // It $.Msg's one line per control -- whether the panel exists, whether it is hit-testable,
    // visible, and how wide it actually laid out. "exists but NOHIT or w=0" is the signature of a
    // control the user reports as unclickable.
    api.renderProbe = function () {
      var ids = ['RsRangeSeg', 'RsCamGrid', 'RsExtSeg', 'RsFormatGrid', 'RsFpsSeg',
                 'RsSizeSeg', 'RsQualitySeg'];
      var out = [];
      for (var i = 0; i < ids.length; i++) {
        var p = null;
        try { p = renderRoot.FindChildTraverse(ids[i]); } catch (e) {}
        out.push(ids[i] + '=' + (p
          ? ((p.hittest ? 'hit' : 'NOHIT') + ',vis' + (p.visible ? 1 : 0)
             + ',w' + Math.round(p.actuallayoutwidth || 0)
             + ',h' + Math.round(p.actuallayoutheight || 0))
          : 'MISSING'));
      }
      var line = out.join('  ');
      try { $.Msg('[renderProbe] ' + line); } catch (e2) {}
      return line;
    };
    api.customizeOptions = function (slot) {
      slot = (slot || '').toString().replace(/^\s+|\s+$/g, '');
      var arr = decoratedOptions(slot), sample = [];
      for (var i = 0; i < arr.length && i < 12; i++) sample.push({ label: arr[i][0], value: arr[i][1], meta: arr[i][2] || {} });
      return JSON.stringify({ slot: slot, count: arr.length, sample: sample });
    };
    api.customizePickFirst = function (slot) {
      slot = (slot || '').toString().replace(/^\s+|\s+$/g, '');
      var arr = decoratedOptions(slot), value = '';
      for (var i = 0; i < arr.length; i++) {
        var v = '' + arr[i][1];
        if (!v || v === 'default' || v === 'ct_default' || v === 't_default') continue;
        if ((slot === 'primary' || slot === 'secondary') && (v === '0' || /:0$/.test(v))) continue;
        if ((slot === 'knife' || slot === 'gloves') && /:0$/.test(v)) continue;
        value = v;
        break;
      }
      if (!value && arr.length) value = '' + arr[0][1];
      if (value) pickCosmetic(slot, value);
      return JSON.stringify({ slot: slot, value: value, sel: custSel, wear: custWear, preview: previewState() });
    };
    // Apply/Cancel/Reset/Clear test hooks for the staged Finish/Wear/Agent/Gloves picks (see
    // markTouched/commitTouchedSlots in CameraEditorCustomizeJs.h) -- lets netcon automation drive the
    // same commit/discard paths the action-bar buttons use, without simulating clicks.
    // Live-tune the modal's F8-style shrink without a rebuild, e.g.
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.customizeScale(80))
    // Returns the applied percent plus previewGeom, so the 3D clip can be checked against the frame
    // in the same call (stage and clip rects must stay 1:1 -- see positionPreview3d).
    api.customizeScale = function (pct) {
      var v = parseFloat(pct);
      if (v > 0) { if (v > 3) v = v / 100; CUST_SCALE = Math.max(0.4, Math.min(1.5, v)); }
      applyCustScale();
      return JSON.stringify({ scale: CUST_SCALE, geom: previewGeom() });
    };
    // Text size multiplier for the modal, relative to the sizes the layout was designed with.
    // Takes effect on the next rebuild of the modal's labels, so it re-injects the UI:
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.customizeFont(120))
    api.customizeFont = function (pct) {
      var v = parseFloat(pct);
      if (!(v > 0)) return 'boost ' + CUST_FONT_BOOST + ' (usage: customizeFont(100..140))';
      if (v > 3) v = v / 100;
      CUST_FONT_BOOST = Math.max(0.7, Math.min(1.6, v));
      return 'boost ' + CUST_FONT_BOOST + ' -- run `mirv_filmmaker ui_rebuild` to apply';
    };
)EDJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"EDJS(
    // Companion to customizeScale: how opaque the modal's surface is, 0..1 (or a percent).
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.customizeOpacity(92))
    api.customizeOpacity = function (pct) {
      var v = parseFloat(pct);
      if (!(v > 0)) return 'usage: customizeOpacity(0..1 | 1..100)';
      if (v > 1) v = v / 100;
      v = Math.max(0.2, Math.min(1, v));
      applyCustOpacity(v);
      return 'surface alpha ' + v;
    };
    // ---- cross-workspace alignment audit ---------------------------------
    // The camera editor, the Config panel and the render workspace are three SEPARATE Panorama
    // scripts, but all three roots are children of the same context panel -- so one probe can
    // measure every tree by id. Geometry does not need script scope, only a reachable panel.
    //
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.uiAudit())
    //
    // Reports each alignment-critical panel as "id=x,y wxh" in CSS px, screen-relative. Panels that
    // are not built or not currently laid out report "-". Two panels that are supposed to share an
    // edge must report the same x (or the same y for a shared baseline) -- that is the whole point:
    // this replaces eyeballing two workspaces side by side with a number you can diff.
    api.uiAudit = function () {
      var sx = ctx.actualuiscale_x || 1, sy = ctx.actualuiscale_y || 1;
      var ids = ['EdHead', 'EdExit', 'EdTabBar', 'EdTabRegular',
                 'CfgHead', 'CfgExit', 'CfgTlBand', 'CfgTlLbl',
                 'RndHead', 'RndBack'];
      var out = [];
      for (var i = 0; i < ids.length; i++) {
        var p = null;
        try { p = ctx.FindChildTraverse(ids[i]); } catch (e) {}
        if (!p || (p.IsValid && !p.IsValid())) { out.push(ids[i] + '=-'); continue; }
        var w = Math.round((p.actuallayoutwidth || 0) / sx);
        var h = Math.round((p.actuallayoutheight || 0) / sy);
        var a = p.GetPositionWithinWindow ? p.GetPositionWithinWindow() : null;
        if (!a || !(w > 0)) { out.push(ids[i] + '=-'); continue; }
        out.push(ids[i] + '=' + Math.round(a.x / sx) + ',' + Math.round(a.y / sy) + ' ' + w + 'x' + h);
      }
      return out.join('  ');
    };
    api.customizeRandomizeAgents = randomizeAgents;
    api.customizeApply = function () { var applied = commitTouchedSlots(); return applied ? 'applied' : 'nothing-to-apply'; };
    api.customizeCancelPending = function () { custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false }; custDirty = false; updateActionBar(); return 'cancelled'; };
    api.customizeResetToDefault = function () { resetToDefault(); return 'reset'; };
    api.customizeState = function () {
      return JSON.stringify({
        visible: !!custOverlay.visible, target: custTargetIndex, key: custTargetKey, name: custTargetName,
        team: normalizeTeamName(custTargetTeam), activeWeaponDef: custActiveWeaponDef, activeWeaponSlot: custActiveWeaponSlot,
        loadout: custLoadout, sel: custSel, wear: custWear, itemWear: custItemWear, preview: previewState(),
        previewGeom: previewGeom(),
        touched: custTouched, dirty: custDirty, applyTarget: applyLabelTarget(),
        pickup: { active: custActiveWeaponPickup, ownerSteamId: custActiveWeaponOwnerSteam, ownerName: custActiveWeaponOwnerName },
        // Slot-level pickup/warning state (entity ownership; survives grenade/knife switches).
        pickupSlots: {
          primary: { pickup: slotPickup('primary'), ownerSteamId: slotOwnerSteam('primary'), ownerName: slotOwnerName('primary') },
          secondary: { pickup: slotPickup('secondary'), ownerSteamId: slotOwnerSteam('secondary'), ownerName: slotOwnerName('secondary') },
          warned: pickupSlots(), banner: !!custPickupBanner.visible
        }
      });
    };
    api.customizeLayout = function () {
      var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      function layoutHeight(panel) { return Math.round(((panel && panel.actuallayoutheight) || 0) / sy); }
      return JSON.stringify({
        windowHeight: layoutHeight(custWin), headerHeight: layoutHeight(custHead), bodyHeight: layoutHeight(custBody),
        pickup: {
          visible: !!custPickupBanner.visible,
          inlineWithName: !!(custPickupBanner.GetParent && custPickupBanner.GetParent() === custTitleWrap),
          height: layoutHeight(custPickupBanner), text: custPickupLbl.text
        }
      });
    };
    api.previewInfo = function () { return previewState(); };
    // The last press the cursor-pipe slider pump saw (cursorSliderPump), and which slider it claimed.
    api.dbgCursorPress = function () { return JSON.stringify({ press: cursorPressDbg, sliders: cursorSliders.length }); };
    // Debug hooks for the customDrop popups (dropdowns are keyed off field.id set in customDrop()):
    //   mirv_filmmaker editor eval "$.Msg($.CamEditor.dbgDropState()+String.fromCharCode(10))"
    //   mirv_filmmaker editor eval "$.Msg($.CamEditor.dbgToggle('FmTargetDrop')+String.fromCharCode(10))"
    //   mirv_filmmaker editor eval "$.Msg($.CamEditor.dbgPick('FmAttachDrop',0)+String.fromCharCode(10))"
    api.dbgDropState = function () {
      var out = { open: openDrop ? openDrop.field.id : null, drops: [] };
      for (var i = 0; i < customDrops.length; i++) {
        var d = customDrops[i];
        out.drops.push({
          id: d.field.id, kind: d.kind || 'compact', opts: d.opts.length, open: !!d.pop.visible,
          ht: !!d.field.hittest, vis: !!d.field.visible,
          height: d.field.style.height, actualHeight: d.field.actuallayoutheight
        });
      }
      return JSON.stringify(out);
    };
    api.dbgToggle = function (id) {
      for (var i = 0; i < customDrops.length; i++) if (customDrops[i].field.id === id) { toggleDrop(customDrops[i]); return 'toggled:' + id; }
      return 'not-found:' + id;
    };
    api.dbgPick = function (id, idx) {
      for (var i = 0; i < customDrops.length; i++) {
        var d = customDrops[i];
        if (d.field.id !== id) continue;
        if (idx < 0 || idx >= d.opts.length) return 'bad-idx:' + d.opts.length;
        var val = d.opts[idx][1];
        closeAllDrops();
        d.onPick(val);
        return 'picked:' + id + ':' + idx;
      }
      return 'not-found:' + id;
    };
    // Index-based variants: customDrop()'s `field.id = id` assignment does not persist through to
    // a readback (confirmed live -- every drop's field.id reads back '' regardless of the string
    // passed to customDrop()), so the id-keyed hooks above can never find anything. Array position
    // is stable across a build (creation order), so index into customDrops directly instead.
    api.dbgToggleAt = function (i) { if (i < 0 || i >= customDrops.length) return 'oob'; toggleDrop(customDrops[i]); return 'toggled:' + i; };
    // Atomic toggle+read (no netcon round-trip gap, so a same-frame revert can't hide behind
    // the ~0.7s/dozens-of-frames gap between two separate netcon commands).
    api.dbgToggleAndState = function (i) {
      if (i < 0 || i >= customDrops.length) return 'oob';
      var d = customDrops[i];
      var beforeWasOpen = (openDrop === d);
      var beforePopVis = !!d.pop.visible;
      toggleDrop(d);
      return JSON.stringify({
        i: i, beforeWasOpen: beforeWasOpen, beforePopVis: beforePopVis,
        afterPopVis: !!d.pop.visible, afterPopHt: !!d.pop.hittest, afterOpenIsThis: (openDrop === d), optCount: d.opts.length
      });
    };
    api.dbgPickAt = function (i, idx) {
      if (i < 0 || i >= customDrops.length) return 'oob';
      var d = customDrops[i];
      if (idx < 0 || idx >= d.opts.length) return 'bad-idx:' + d.opts.length;
      var val = d.opts[idx][1];
      closeAllDrops();
      d.onPick(val);
      return 'picked:' + i + ':' + idx;
    };
    // Debug hooks for the gear settings fly-out (mirrors the previewInfo/previewCall pattern):
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.toggleSettings()+String.fromCharCode(10));
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.settingsGeom()+String.fromCharCode(10));
    api.toggleSettings = function () { toggleSettings(); return settingsOverlay.visible ? 'open' : 'closed'; };
    api.settingsGeom = function () {
      var p = settingsCard.GetPositionWithinWindow ? settingsCard.GetPositionWithinWindow() : null;
      var gp = gearBtn.GetPositionWithinWindow ? gearBtn.GetPositionWithinWindow() : null;
      return JSON.stringify({
        ov: !!settingsOverlay.visible, cv: !!settingsCard.visible,
        mb: settingsCard.style.marginBottom, mr: settingsCard.style.marginRight,
        card: p ? { x: p.x, y: p.y, w: p.width, h: p.height } : null,
        gear: gp ? { x: gp.x, y: gp.y, w: gp.width, h: gp.height } : null
      });
    };
    api.previewFit = function (json) { return previewFit(json); };
    // Diagnostic for the paused-preview render investigation: reports what the native preview panel
    // itself thinks its state is, and dumps its composition layer to a PNG. This distinguishes the
    // two possible causes of the black box while the demo is paused -- a layer that was never
    // RENDERED (PNG comes out empty) versus a rendered layer that is not being DISPLAYED (PNG has the
    // character). See Docs/panorama/panorama-player-preview-summary.md.
    api.previewLayer = function (name) {
      if (!preview3d || !preview3d.IsValid || !preview3d.IsValid()) return 'nopanel';
      var s = 'rdy=' + preview3d.BReadyForDisplay() +
              ' w=' + preview3d.actuallayoutwidth + ' h=' + preview3d.actuallayoutheight +
              ' vis=' + preview3d.visible;
      try {
        preview3d.WriteCompositionLayerPNG(name || 'mvm_previewlayer');
        s += ' wrote=' + (name || 'mvm_previewlayer');
      } catch (e) { s += ' writeErr=' + e; }
      return s;
    };
    // Click-and-drag preview repositioning (user-driven, live): drag inside the preview box to nudge
    // the character, then call previewSave('start') while zoomed out or previewSave('zoom') while
    // zoomed in to bake it; previewPanReset() discards an in-progress drag without saving.
    api.previewSave = function (which) { return previewSavePosition(which); };
    api.previewPanReset = function () { return previewPanReset(); };
    // Live A/B of the preview SCENE preset (0=match_mvp, 1=vanity, 2=loadout-grid) without a
    // rebuild, so the HUD-compatible scene can be confirmed by eye over netcon:
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.previewTry(0)+String.fromCharCode(10));
    api.previewTry = function (idx) {
      previewSceneIdx = (idx | 0);
      USE_3D_PREVIEW = true; // force-enable for live experimentation (off by default in the HUD)
      try { if (preview3d && preview3d.DeleteAsync) preview3d.DeleteAsync(0); } catch (e) {}
      preview3d = null; preview3dTried = false; previewModelKey = '';
      pokePreviewSoon();
      applyPreview();
      return 'scene=' + previewSceneIdx + ' map=' + currentScene().map + ' ' + previewState();
    };
)EDJS"
R"EDJS(
    // Destroy + recreate the preview with override attrs (JSON), e.g. to try a different map/camera:
    //   mirv_filmmaker editor eval $.Msg($.CamEditor.previewRebuild('{"map":"ui/match_mvp","camera":"camera"}')+String.fromCharCode(10));
    api.previewRebuild = function (json) {
      var ov = null; try { ov = json ? JSON.parse(json) : null; } catch (e) { return 'bad json'; }
      try { if (preview3d && preview3d.DeleteAsync) preview3d.DeleteAsync(0); } catch (e) {}
      try { if (previewItem3d && previewItem3d.DeleteAsync) previewItem3d.DeleteAsync(0); } catch (e) {}
      preview3d = createPreview3d(ov); previewItem3d = null; preview3dTried = true;
      previewModelKey = (ov && ov.playermodel) ? ov.playermodel : '';
      preview2d.visible = !preview3d;
      if (preview3d) applyPreview();
      return previewState();
    };
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      // Track the operator's column width before anything lays out against it this frame. While
      // the grip is being dragged it owns the value and the stored one is stale by a frame.
      if (!edGripBusy && typeof st.inspW === 'number' && st.inspW > 0) edInspW = Math.floor(st.inspW);
      if (!editorUiHydrated) {
        inspectorMode = (st.uiEditorInspector === 'follow') ? 'follow'
          : ((st.uiEditorInspector === 'scene') ? 'scene' : 'path');
        // Each tab's selected rail card and each card's sub-page. An absent or unknown key keeps
        // the first card. (Scene Keys' catalog cards arrive later; their selection is re-applied
        // by syncSceneInspector once they exist.)
        var eui = st.ui || {};
        for (var ek in edRails) {
          var want = eui['editor.' + ek + '.sel'];
          if (want) edSelectCard(ek, want, false);
        }
        for (var ec = 0; ec < edCards.length; ec++) {
          var pk = edCards[ec].__pageKey;
          if (pk && eui[pk] !== undefined) edCards[ec].__setPage(eui[pk]);
        }
        editorUiHydrated = true;
      }

      // Capture chrome gate: while a render job is capturing with a before-present pass, every
      // Source MVM panel must vanish or it gets baked into the output. Projected per frame, so
      // the chrome comes straight back the frame the job leaves the capture states.
      if (st.render && st.render.captureHide) {
        root.visible = false;
        if (renderRoot) renderRoot.visible = false;
        // The customize modal and its confirm are their OWN roots (CameraEditorCustomizeJs.h), so
        // hiding the editor root no longer hides them -- they would bake straight into the capture.
        closeCustomize();
        return;
      }

      root.visible = !!(st.enabled || st.debug);
      if (!st.enabled) {
	closeAllDrops();
	closeSettings('notEnabled');
	closeCustomize();
	if (renderRoot) renderRoot.visible = false;
	customizeBtn.visible = false;
	renderBtnEl.visible = false;
	actionRow.visible = false;
        confirmOverlay.visible = false;
        catcherTop.hittest = catcherBottom.hittest = catcherLeft.hittest = catcherRight.hittest = false;
        tag.visible = false;
        backdrop.visible = false;
        tabBar.visible = false;
        inspector.visible = false;
        // The drag edge belongs to the column: no column on screen, no edge to drag.
        edGrip.panel.visible = false;
        barLeft.visible = barRight.visible = barTop.visible = barBottom.visible = false;
        frameL.visible = frameV.visible = frameT.visible = frameH.visible = false;
        var crsx = root.actualuiscale_x || ctx.actualuiscale_x || 1, crsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
        var crawW = root.actuallayoutwidth || 0, crawH = root.actuallayoutheight || 0;
        if (crawW < 16) { crawW = ctx.actuallayoutwidth || 0; crawH = ctx.actuallayoutheight || 0; }
        var crw = crawW / crsx, crh = crawH / crsy;
        dbgGeom = (crw > 1 && crh > 1) ? { x0: 0, y0: 0, x1: 1, y1: 1, ox: 0, oy: 0, pw: crw, ph: crh } : null;
        root.SetAttributeString('previewrect', '0 0 1 1');
        updateDebug(st);
        return;
      }
      inspector.visible = true;
      tabBar.visible = true;
      // Pulse on a token change. A counter, so holding F and pressing again both read as new.
      if (st.flashToken != null && st.flashToken !== flashSeen) {
        if (flashSeen >= 0) pulseInspector();
        flashSeen = st.flashToken;
      }

      // UI-cursor gating: panels are only clickable in UI-mouse mode; in GAME mode the
      // mouse flies the free cam and Panorama receives no clicks anyway.
      var cur = !!st.cursor;
      if (!cur) { closeAllDrops(); closeSettings('noCursor'); closeCustomize(); } // can't click popups in GAME-mouse mode; don't leave them hanging
      repositionOpenDrop(); // keep an open popup glued to its field while a scroll container moves it
      custScrollMaintain();  // size + clamp the modal's manual-scroll viewports (right column + open dropdown)
      // The column's drag edge, before every other pointer consumer: while the edge owns the
      // pointer neither a preview pan nor an inspector slider may also claim it.
      try { edGripBusy = edGrip.pump(st); } catch (e) { edGripBusy = false; }
      previewUpdateDrag();   // click-and-drag preview panning (reads the live OS-cursor pipe in st.mx/my/lmb)
      cursorSliderPump();    // inspector slider drags: relative, damped, survive leaving the panel
      publishCustomizeOpenState(); // safety net: covers visibility flips that skip open/closeCustomize
      catcherTop.hittest = catcherBottom.hittest = catcherLeft.hittest = catcherRight.hittest = cur;
      inspector.hittest = cur;
      backdrop.hittest = cur;
      tabBar.hittest = cur;

      // Customize button: always visible while the editor is open so the player can always
      // inspect/change weapon skins and loadout regardless of the free-cam state. The modal
      // itself handles the "no spectated target" edge case.
	customizeBtn.visible = true;
	// Both actions share one aligned row. The workspace replaces this launcher row while open.
	var renderWorkspaceOpen = !!(st.render && st.render.open);
	var renderWsChanged = (renderWorkspaceOpen !== renderWsSeen);
	if (renderWsChanged) renderWsSeen = renderWorkspaceOpen;
	var workspaceAccent = renderWorkspaceOpen ? S.render : S.accent;
	actionRow.visible = !renderWorkspaceOpen;
	renderBtnEl.visible = true;
	inspector.visible = !renderWorkspaceOpen;
	// THE DRAG EDGE GOES WITH THE COLUMN. The grip is its own panel parented to the root, so
	// hiding the inspector left it behind: in the Render workspace -- which has its own JOB and
	// SETTINGS columns and no draggable inspector -- the editor's amber rule went on painting
	// down the middle of the screen, over the preview, and could still be grabbed and dragged.
	edGrip.panel.visible = !renderWorkspaceOpen;
	// Entering the Render workspace swaps the whole right-hand column and adds a left one; both
	// slide in together so the change reads as ONE workspace arriving rather than two unrelated
	// slabs appearing. Fired AFTER the visibility writes above on purpose -- an entrance written
	// to a still-hidden panel has no layout pass to animate from, so it would simply snap.
	// One-shot: render() runs every frame.
	if (renderWsChanged) {
		if (renderWorkspaceOpen) { moSlideIn(renderSettingsCol, 12); moSlideIn(renderJobCol, 12); }
		else moSlideIn(inspector, 12);
	}
	if (st.render) {
		renderBtnLbl.text = st.render.jobActive ? 'RENDER PROGRESS' : 'RENDER';
		btnSel(renderBtnEl, st.render.jobActive);
	}
      var ff = st.follow || {};
      var prevTargetKey = custTargetKey;
      var ct = st.customizeTarget || null;
      custTargetIndex = (typeof st.obsTarget === 'number' && st.obsTarget >= 0) ? st.obsTarget : -1;
      custTargetKey = custTargetIndex >= 0 ? ('pawn:' + custTargetIndex) : 'pawn:-1';
      custActiveWeaponDef = 0;
      custActiveWeaponSlot = '';
      custActiveWeaponPickup = false;
      custActiveWeaponOwnerSteam = '';
      custActiveWeaponOwnerName = '';
      if (ct && typeof ct.pawnIndex === 'number' && ct.pawnIndex >= 0) {
        custTargetIndex = ct.pawnIndex;
        custTargetKey = ct.key || ('pawn:' + ct.pawnIndex);
        custTargetName = ct.name || ('Player #' + ct.pawnIndex);
        custTargetTeam = ct.team;
        custActiveWeaponDef = parseInt(ct.activeWeaponDefIndex || 0, 10) || 0;
        custActiveWeaponSlot = WEAPON_SLOT_BY_DEF[custActiveWeaponDef] || '';
        custActiveWeaponPickup = !!ct.activeWeaponPickup;
        custActiveWeaponOwnerSteam = ct.activeWeaponOwnerSteamId ? ('' + ct.activeWeaponOwnerSteamId) : '';
        custActiveWeaponOwnerName = ct.activeWeaponOwnerName ? ('' + ct.activeWeaponOwnerName) : '';
      }
      if (custTargetIndex < 0 && typeof ff.targetIndex === 'number' && ff.targetIndex >= 0) custTargetIndex = ff.targetIndex;
      var cand = ff.candidates || [];
      if (custTargetIndex < 0 && cand.length) custTargetIndex = cand[0].index; // best demo fallback
      if (!ct && custTargetIndex >= 0 && st.customizePlayers)
        ct = st.customizePlayers['' + custTargetIndex] || null;
      if (ct && typeof ct.pawnIndex === 'number' && ct.pawnIndex >= 0) {
        custTargetIndex = ct.pawnIndex;
        custTargetKey = ct.key || ('pawn:' + ct.pawnIndex);
        custTargetName = ct.name || ('Player #' + ct.pawnIndex);
        custTargetTeam = ct.team;
        custActiveWeaponDef = parseInt(ct.activeWeaponDefIndex || 0, 10) || 0;
        custActiveWeaponSlot = WEAPON_SLOT_BY_DEF[custActiveWeaponDef] || '';
        custActiveWeaponPickup = !!ct.activeWeaponPickup;
        custActiveWeaponOwnerSteam = ct.activeWeaponOwnerSteamId ? ('' + ct.activeWeaponOwnerSteamId) : '';
        custActiveWeaponOwnerName = ct.activeWeaponOwnerName ? ('' + ct.activeWeaponOwnerName) : '';
      }
      if (!(ct && typeof ct.pawnIndex === 'number' && ct.pawnIndex >= 0))
        custTargetKey = custTargetIndex >= 0 ? ('pawn:' + custTargetIndex) : 'pawn:-1';
      var nm = ct && ct.name ? ct.name : null, tm = (ct && ct.team !== undefined) ? ct.team : '';
      if (!ct) for (var ci = 0; ci < cand.length; ci++) if (cand[ci].index === custTargetIndex) { nm = cand[ci].name; tm = cand[ci].team || ''; break; }
      custTargetName = nm || (custTargetIndex >= 0 ? ('Player #' + custTargetIndex) : 'Player');
      custTargetTeam = tm;
      applyCustomizeTargetLoadout(ct);
      updatePickupBanner();
      var nextLoadoutSig = customizeLoadoutSignature();
      if (custOverlay.visible && (custTargetKey !== prevTargetKey || custDisplayedLoadoutSig !== nextLoadoutSig)) {
        // Target/loadout changed under the open modal (player switch, or the demo's own state moved
        // on) -- discard any unstaged picks and reseed from the live loadout, same as opening fresh.
        previewLog('reseed', (custTargetKey !== prevTargetKey ? 'target' : 'loadout') + ' dirty=' + (custDirty ? 1 : 0)
          + ' was=' + custDisplayedLoadoutSig + ' now=' + nextLoadoutSig);
        custTouched = { agent: false, primary: false, secondary: false, knife: false, gloves: false };
        custDirty = false;
        resetSelectionsFromLoadout();
        custTitleName.text = (custTargetName || 'PLAYER').toUpperCase();
        populateCustomize();
        // The reseed picked the slot the new loadout holds; the always-open picker follows it.
        selectSlot(custActiveCategory);
        updatePreview();
        pokePreviewSoon();
        custDisplayedLoadoutSig = nextLoadoutSig;
      }
      maintainPreview(); // self-heals the 3D preview for a few frames after it (re)opens

      var followMode = inspectorMode === 'follow';
      var sceneMode = inspectorMode === 'scene';
      var pathMode = !followMode && !sceneMode;
      edApplyRails(inspectorMode);
      edFollowBar.visible = followMode;
      edKeyStrip.visible = pathMode;
      styleTab(followTab, followMode, workspaceAccent);
      styleTab(pathTab, pathMode, workspaceAccent);
      styleTab(sceneTab, sceneMode, workspaceAccent);
      // A tab change starts the pane at its top, and the new tab ARRIVES: the rail, the tab's
      // selected card and (on Path) the key strip rise over the shared MO_TAB. Once per real
      // change, never per frame.
      if (inspectorTabSeen !== inspectorMode) {
        inspectorTabSeen = inspectorMode; edScrollY = 0;
        edRailScrollY = 0; edRailAppliedY = -1;   // each tab's rail starts at its top
        moEnter(edRailHost, UI.mo.rise, MO_TAB);
        var tr = edRails[inspectorMode];
        if (tr && tr.sel && tr.items[tr.sel]) moEnter(tr.items[tr.sel].card, UI.mo.rise, MO_TAB);
        if (pathMode) moEnter(edKeyStrip, UI.mo.rise, MO_TAB);
        if (followMode) moEnter(edFollowBar, UI.mo.rise, MO_TAB);
      }
      var bm = st.bottomMode || (st.graphExp ? 'timeline' : 'native');
      curBottomMode = bm;
      var regularActive = (bm !== 'timeline'); // 'native' (Regular Timeline)
      // The ribbon owns its own tab painting; this panel only says which tab is live and what the
      // TIMELINE dock reports about itself (its layer and key counts).
      dockRibbon.sync(bm, bm === 'timeline' ? mvmTimelineMeta() : '');
      // The gear opens the CAM-EDITOR settings fly-out (native demo-playback toggles). It has no
      // business in the Render workspace, which is a different job with its own tabbed settings --
      // and the fly-out is closed here rather than merely hidden, so switching into Render cannot
      // strand it open over the render UI.
      gearBtn.visible = !renderWorkspaceOpen;
      if (renderWorkspaceOpen && settingsOverlay.visible) closeSettings('renderWorkspace');
      // The accent rules read as a deliberate gold frame in the Camera Editor, but the Render
      // workspace's accent is near-white (#aeb4bc), so the same bright line across the top of the
      // Regular Timeline / Camera / Graph toolbar looked like a stray artefact there. Render gets
      // a QUIET divider instead of nothing: with no rule at all the tab strip and the bar beneath
      // it read as one undifferentiated slab.
      tabBar.style.borderTop = '1px solid ' + (renderWorkspaceOpen ? '#ffffff26' : workspaceAccent);
      tabBar.style.borderBottom = '1px solid ' + (renderWorkspaceOpen ? '#ffffff14' : workspaceAccent);
)EDJS"
R"EDJS(
      // The LIVE / PREVIEW badge belongs to the workspace chrome, not to the follow card.
      var liveBadge = !!((st.follow && st.follow.enabled) || st.pathLive || (st.graphExp && st.graphDrive));
      tagLbl.text = liveBadge ? 'LIVE' : 'PREVIEW';
      // The FOLLOW tab fills itself: syncFollowInspector lives in
      // CameraEditorFollowInspectorJs.h, beside the controls it writes.
      syncFollowInspector();
      // SCENE KEYS fills itself the same way.
      syncSceneInspector();

      // Aspect-ratio letterbox. Virtual px = actuallayout / uiscale (matches style px).
      // Measure from the ALREADY-laid-out HUD context panel when our fresh root still reports 0
      // (the first layout passes after a build / resolution switch) -- otherwise the preview +
      // inspector squish into the top-left for ~half a second until root settles.
      var rsx = root.actualuiscale_x || ctx.actualuiscale_x || 1, rsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var rawW = root.actuallayoutwidth || 0, rawH = root.actuallayoutheight || 0;
      if (rawW < 16) { rawW = ctx.actuallayoutwidth || 0; rawH = ctx.actuallayoutheight || 0; }
      var rw = rawW / rsx, rh = rawH / rsy;
      var inspectorW = responsiveInspectorW(rw);
      if (inspectorW !== edLayoutW) { edLayoutW = inspectorW; edRelayout(); }
      edGrip.place(inspectorW);
      inspector.style.width = inspectorW + 'px';
      // RENDER / CUSTOMIZE PLAYER: built once from UI.col.w, so they need re-sizing with the rest.
      actionRow.style.width = Math.floor(inspectorW - UI.action.inset * 2) + 'px';
      footerBg.style.width = inspectorW + 'px';
      columnGround.sync(inspectorW, inspector.visible);
      bottomFade.style.width = inspectorW + 'px';
      topFade.style.width = inspectorW + 'px';
      // The tab bar + backdrop span the content width left of the inspector. The tab bar is a
      // DIVIDER docked directly ABOVE the active bottom editor; the editor rests flush at the
      // bottom and the preview shrinks to clear both. Anchor from the top; Panorama does not
      // reliably apply marginBottom on this full-screen root after the viewport is scaled.
      tabBar.style.width = Math.floor(Math.max(0, rw - (renderWorkspaceOpen ? 0 : inspectorW))) + 'px';
      // SHORTEN THE SCROLL VIEWPORT, do not pad it.
      //
      // RENDER / CUSTOMIZE PLAYER float over the inspector at zIndex 120. The inspector used to be
      // height:100% with 76px of paddingBottom to clear them -- but padding only reserves space at
      // the END of the scroll CONTENT, so anything scrolled into the bottom 76px of the viewport
      // still rendered underneath the buttons. With Advanced Tracking open that is a slider and a
      // toggle sitting behind an opaque gold button, which reads as the panel being cut off.
      // Ending the viewport above the action row is the only thing that actually moves content up.
      if (finitePositive(rh)) {
        var inspH = Math.max(160, Math.floor(rh - (actionRow.visible ? ACTION_CLEAR : 0)));
        if (inspH !== lastInspectorH) { lastInspectorH = inspH; inspector.style.height = inspH + 'px'; }
      }
      footerBg.visible = actionRow.visible && !renderWorkspaceOpen;
      // Only while content actually runs under it: over the textured column ground a fade with
      // nothing to dissolve is just a dark strip above the buttons.
      bottomFade.visible = footerBg.visible && (edContentH() - edScrollY > edViewH() - FADE_H + 1);
      // Only while there is something above to fade INTO.
      topFade.visible = footerBg.visible && edScrollY > 1;
      topFade.style.marginTop = edScrollTop() + 'px';
      edApplyScroll();   // re-clamps + re-places the thumb as the column's content changes height
      edRailApplyScroll();   // same for the rail: fewer cameras means less travel
      if (!finitePositive(rw) || !finitePositive(rh)) {
        hidePreviewLayout();
      } else {
      // editorTop = y-from-bottom of the TOP edge of whatever editor is docked below the tab bar.
      // Camera/Graph overlays rest at marginBottom=BOTTOM_LIFT; the native bar rests at the very
      // bottom (Regular Timeline mode). Measure live heights where they can grow.
      var nativeBottom = (bm !== 'timeline'); // 'native' = Regular Timeline
)EDJS"
R"EDJS(
      // One shared measurement with CONFIG (mvmDockMeasure, TimelineDockRibbonJs.h): the
      // docked editor's real height, the band above it, and where the ribbon sits.
      var dm = mvmDockMeasure(ctx, rh, rsy, bm, { floor: BOTTOM_LIFT });
      var editorTop = dm.editorTop, bottomH = dm.bottomH, dockTopY = dm.dockTopY, tabY = dm.tabY;
      tabBar.style.position = '0px 0px 0px';
      tabBar.style.marginTop = Math.floor(tabY) + 'px';
      tabBar.style.marginBottom = '0px';
      // Keep the gear settings card glued right above the gear button: bottom edge just above the
      // tab bar's top (which moves with whichever editor is docked below), right edge aligned to
      // the bar's right end (the bar stops at the inspector, so a plain screen-right anchor would
      // put the card over the inspector instead of over the gear).
      // CLAMP to keep the whole card on-screen: the card is bottom-anchored and grows UPWARD, so when
      // a tall editor is docked below (e.g. the graph editor) an unclamped "glue above the tab bar"
      // marginBottom pushed the card's TOP off the top of the screen -- the gear toggled the overlay
      // visible but nothing was on screen ("gear reacts but no panel appears"). Cap marginBottom so
      // the card top stays within the viewport, using its real laid-out height when available.
      var scMB = Math.max(0, rh - tabY) + 8;
      var scH = (settingsCard.actuallayoutheight || 0) / rsy;
      if (scH > 8 && rh > scH + 16) scMB = Math.min(scMB, rh - scH - 8);
      settingsCard.style.marginBottom = Math.floor(Math.max(8, scMB)) + 'px';
      settingsCard.style.marginRight = (inspectorW + 16) + 'px';
      // In Regular Timeline mode the native CS2 bar (a LOWER-z HUD root) is the editor: our opaque
      // backdrop (z53) would HIDE the bar if it covered it, and our full-screen catcher would eat the
      // bar's clicks -- so the catcher shrinks to the preview area (the scaler blacks the rest).
      // For the backdrop we DON'T drop it entirely (that left the TAB_GAP strip just above the bar
      // see-through, bleeding the game render as a thin line); instead we anchor it ABOVE the bar
      // (editorTop..bottomH) so it fills that gap behind the tab bar without covering the native bar.
      // Camera/graph overlays render ABOVE z53, so there the backdrop covers the whole band (0..bottomH).
      backdrop.style.position = '0px 0px 0px';
      backdrop.style.marginTop = Math.floor(dockTopY) + 'px';
      backdrop.style.marginBottom = '0px';
      if (nativeBottom) {
        backdrop.style.height = Math.floor(Math.max(0, bottomH - editorTop)) + 'px';
      } else {
        backdrop.style.height = Math.floor(bottomH) + 'px';
      }
      backdrop.visible = true;
      // RENDER WORKSPACE SIZING.
      //
      // The Render workspace is THREE columns: a job summary on the left, the preview in the
      // middle, a settings inspector on the right (CameraEditorRenderLayoutJs.h owns the columns
      // themselves). So its preview area is the horizontal BAND between the two columns, running
      // the full height down to the bottom tab bar -- which means changing the bottom editor
      // (Regular Timeline / Camera / Graph moves tabY) rescales the preview and both columns
      // together, exactly as it does for the normal editor.
      //
      // This replaced a preview-on-top / full-width-dock-below arrangement whose dock stole the
      // lower half of the screen and left the preview small and top-pinned.
      var areaX0 = renderWorkspaceOpen ? Math.min(RENDER_JOB_W, Math.floor(rw * 0.30)) : 0;
      var areaW = renderWorkspaceOpen
        ? Math.max(0, rw - areaX0 - Math.min(RENDER_SETTINGS_W, Math.floor(rw * 0.34)))
        : rw - inspectorW;
      var areaH = renderWorkspaceOpen ? tabY : rh - bottomH;
      if (finitePositive(areaW) && finitePositive(areaH)) {
        var aspect = rw / rh;            // the game's render aspect (full window)
        var pw = areaW, ph = pw / aspect;
        if (ph > areaH) { ph = areaH; pw = ph * aspect; } // fit the rect inside the area
        pw = Math.floor(pw); ph = Math.floor(ph);
        if (!finitePositive(pw) || !finitePositive(ph)) {
          hidePreviewLayout();
        } else {
        // Centre the shrunk preview in the available area so the black letterbox is even on
        // all sides instead of pinned to one edge with one fat gap.
        var ox = areaX0 + Math.floor((areaW - pw) / 2); if (ox < areaX0) ox = areaX0;
        var oy = Math.floor((areaH - ph) / 2); if (oy < 0) oy = 0;
        var rgt = ox + pw, bot = oy + ph; // preview right / bottom edges (root space)
        if (renderWorkspaceOpen) {
          layoutRenderWorkspace(tabY, rw, cur, ox, oy, rgt, bot);
        } else {
          root.SetAttributeString('renderhit', 'off');
        }

        // The masks are placed by shell.layout below; only the invariant still reads this.
        var maskSideH = areaH;
)EDJS"
R"EDJS(
        // ---- INVARIANTS: prevent future layout regressions across Config/Render/Cam paths ----
        // Rate-limited: fires at most once per unique violation signature, resets on HUD rebuild.
        (function checkLayoutInvariants() {
          var bad = [];
          var rw_ = rw, rh_ = rh, areaW_ = areaW, areaH_ = areaH, areaX0_ = areaX0;
          var ox_ = ox, oy_ = oy, rgt_ = rgt, bot_ = bot, pw_ = pw, ph_ = ph;
          var rwOpen = !!renderWorkspaceOpen;
          // The area no longer always starts at x=0: the Render workspace's preview band sits
          // BETWEEN the job column and the settings column, so every horizontal bound is against
          // [areaX0, areaX0 + areaW] rather than [0, areaW].
          var areaRight_ = areaX0_ + areaW_;
          // 1. Preview rect must be a positive rectangle.
          if (rgt_ <= ox_ + 1) bad.push('preview width degenerate: rgt=' + rgt_ + ' ox=' + ox_);
          if (bot_ <= oy_ + 1) bad.push('preview height degenerate: bot=' + bot_ + ' oy=' + oy_);
          // 2. Width/height must be self-consistent (allow 1px floor rounding).
          if (Math.abs(rgt_ - ox_ - pw_) > 1) bad.push('pw mismatch: pw=' + pw_ + ' computed=' + (rgt_ - ox_));
          if (Math.abs(bot_ - oy_ - ph_) > 1) bad.push('ph mismatch: ph=' + ph_ + ' computed=' + (bot_ - oy_));
          // 3. Preview must fit inside the area, both axes, in every mode (it is centred now).
          if (bot_ > areaH_ + 1) bad.push('bot exceeds areaH: bot=' + bot_ + ' areaH=' + areaH_);
          if (ox_ < areaX0_ - 1) bad.push('ox left of the area: ox=' + ox_ + ' areaX0=' + areaX0_);
          if (rgt_ > areaRight_ + 1) bad.push('rgt exceeds the area: rgt=' + rgt_ + ' areaRight=' + areaRight_);
          // 4. The side masks span the full area height in every mode now (the preview is centred,
          //    so nothing can stick out below them the way the old top-docked layout could).
          if (maskSideH != areaH_) bad.push('maskSideH must equal areaH: maskSideH=' + maskSideH + ' areaH=' + areaH_);
          // 5. In the Render workspace the area must actually leave room between the columns.
          if (rwOpen && areaW_ < 240) bad.push('render preview band too narrow: areaW=' + areaW_ + ' (columns too wide for rw=' + rw_ + ')');
          // 7. Fractions published to previewrect must be in [0,1].
          var fx0 = ox_ / rw_, fy0 = oy_ / rh_, fx1 = rgt_ / rw_, fy1 = bot_ / rh_;
          if (fx0 < -0.001 || fx0 > 1.001) bad.push('x0 out of range: ' + fx0.toFixed(4));
          if (fy0 < -0.001 || fy0 > 1.001) bad.push('y0 out of range: ' + fy0.toFixed(4));
          if (fx1 < -0.001 || fx1 > 1.001) bad.push('x1 out of range: ' + fx1.toFixed(4));
          if (fy1 < -0.001 || fy1 > 1.001) bad.push('y1 out of range: ' + fy1.toFixed(4));
          if (fx1 - fx0 <= 0.001 || fy1 - fy0 <= 0.001)
            bad.push('preview fraction span too small: dx=' + (fx1 - fx0).toFixed(4) + ' dy=' + (fy1 - fy0).toFixed(4));
          if (bad.length) {
            // Log once per unique signature, not every frame.
            var sig = bad.join('|') + '|rwOpen=' + rwOpen;
            if (sig !== _lastInvariantSig) {
              _lastInvariantSig = sig;
              for (var bi = 0; bi < bad.length; bi++)
                $.Msg('[editor] INVARIANT BROKEN: ' + bad[bi] + '\n');
            }
          } else {
            _lastInvariantSig = '';
          }
        })();
        // In Regular Timeline mode the bottom catcher stops at the native bar's top edge so the
        // bar keeps its own clicks; the camera/graph overlays are higher-z roots and win anyway.
        // editorTop IS the measured native bar height in Regular Timeline mode (mvmDockMeasure),
        // which is what the old local natH held before the measurement moved into the shell.
        var cbBottom = nativeBottom ? Math.max(0, rh - editorTop) : rh;
        shell.layout({ rw: rw, rh: rh, areaX0: areaX0, areaW: areaW, areaH: areaH,
                       ox: ox, oy: oy, pw: pw, ph: ph, cursorOn: cur, catcherBottom: cbBottom,
                       accent: workspaceAccent,
                       title: renderWorkspaceOpen ? 'RENDER / OUTPUT' : 'CAMERA / EDITOR',
                       code: renderWorkspaceOpen ? 'SOURCE // FINAL FRAME' : 'SOURCE // MVM FILMMAKER',
                       // Render's accent is near-white and the preview sits flush against two
                       // opaque columns, so a full-strength frame reads as a seam there.
                       frameColor: renderWorkspaceOpen ? (S.render.substring(0, 7) + '66') : workspaceAccent });


        // Publish the preview rect as NORMALISED root fractions (x0 y0 x1 y1 corners) so the
        // render-layer scaler (CViewportScaler) blits the whole frame into exactly this centred
        // rect when "editor scale" is on (it clears the rest black). Single source of truth
        // shared between Panorama (virtual px) and D3D (physical px); fractions stay resolution/
        // uiscale-independent so the two passes line up. The bars double as the visual frame
        // when scaling is OFF (crop mode); when ON they sit black-on-black.
        root.SetAttributeString('previewrect',
          (ox / rw).toFixed(5) + ' ' + (oy / rh).toFixed(5) + ' ' + (rgt / rw).toFixed(5) + ' ' + (bot / rh).toFixed(5));
        // [debug overlay] stash the laid-out preview geometry (fractions + virtual px) for updateDebug.
        dbgGeom = { x0: ox / rw, y0: oy / rh, x1: rgt / rw, y1: bot / rh, ox: ox, oy: oy, pw: pw, ph: ph };
        }
      } else {
        hidePreviewLayout();
      }
      }

      // The PATH tab fills itself: syncPathInspector lives in CameraEditorPathInspectorJs.h,
      // beside the controls it writes, so render() has nothing to say about what that tab holds.
      syncPathInspector();

	updateDebug(st); // viewport/HUD debug overlay (no-op unless st.debug)
	renderRender();
    };

    // Called from C++ (CameraEditorHud::DispatchWheel) with the notches captured this frame:
    // +1 = wheel up. Same notch size as the Config panel, so the two workspaces scroll identically.
    api.wheel = function (notches) {
      var n = parseInt(notches, 10);
      if (!isFinite(n) || n === 0) return;
      // The wheel moves content out from under a stationary cursor and Panorama fires no mouseout
      // for that, so a tooltip opened before the scroll would hang over controls it no longer
      // belongs to. Drop it with the first notch.
      hideTip();
      // The graph dock gets first refusal: with SCENE KEYS open it stacks one strip per property
      // and that stack is what the wheel has to move when the pointer is over it. It answers false
      // everywhere else, so the column keeps the wheel in every other case.
      try { if ($.GraphExp && $.GraphExp.wheel && $.GraphExp.wheel(n)) return; } catch (e) {}
      // The rail takes the notch while the pointer is over it and its rows overflow the column.
      if (edRailWheel(n)) return;
      // (api.railWheel below scrolls the rail without the pointer test, for automation.)
      // A notch over a value tile was proposed to edit that tile (2% of its range, 0.4% with Shift)
      // rather than scroll -- the original design, superseded by Docs/panorama/panorama_ui_guide.md's
      // scroll-only wheel contract shared with the Config panel.
      edScrollY -= n * 90;
      edApplyScroll();
    };
    // ---- automation hooks ----------------------------------------------------------------
    // Scroll the active rail by notches without the pointer-over test (netcon cannot hover).
    api.railWheel = function (notches) {
      var n = parseInt(notches, 10);
      if (!isFinite(n) || n === 0) return edRailScrollY;
      edRailScrollY -= n * 90;
      edRailApplyScroll();
      return edRailScrollY;
    };
    // Same reason as the Config panel's: synthetic mouse input does not reach CS2, and
    // st.uiEditorInspector is only read once during hydration, so the stored setting alone moves
    // nothing on screen.
    // ARGUMENTS ARRIVE PADDED. CS2's console tokenizer splits on quotes and commas, and the
    // `... eval <js>` verbs rejoin ArgV with a single space -- so `gotoTab('camera')` reaches the
    // script as `gotoTab(' camera ')`. Harmless inside JS syntax, fatal for a string compare, so
    // every console-driven helper trims what it is given.
    function evalArg(v) { return ('' + (v === undefined || v === null ? '' : v)).replace(/^\s+|\s+$/g, ''); }
    api.gotoInspector = function (which) {
      which = evalArg(which);
      inspectorMode = (which === 'follow') ? 'follow' : ((which === 'scene') ? 'scene' : 'path');
      cmd('mirv_filmmaker settings ui editor.inspector ' + inspectorMode);
      api.render();
      return inspectorMode;
    };
    // Open or shut one inspector card by title, and pick its sub-page by index.
    api.card = function (title, open) {
      title = evalArg(title);
      for (var i = 0; i < edCards.length; i++) {
        if (edCards[i].__title !== title) continue;
        edCards[i].__setOpen(!(open === '0' || open === 0 || open === false));
        return title + '=' + (edCards[i].__getOpen() ? 'open' : 'shut');
      }
      return 'unknown:' + title;
    };
    api.cardPage = function (title, index) {
      title = evalArg(title);
      for (var i = 0; i < edCards.length; i++) {
        if (edCards[i].__title !== title) continue;
        edCards[i].__setPage(index);
        return title + '#' + index;
      }
      return 'unknown:' + title;
    };
    $.CamEditor = api;
    api.render();
    $.Msg('[editor] camera editor workspace built.\n');
  } catch (err) {
    $.Msg('[editor] gui error: ' + err + '\n');
  }
})();
)EDJS";

} // namespace Filmmaker
