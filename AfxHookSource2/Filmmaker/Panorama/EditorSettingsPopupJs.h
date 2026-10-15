#pragma once

// EDITOR SETTINGS fly-out -- ONE implementation, spliced into TWO host scripts.
//
// The popup mirrors CS2's native demo-playback settings (huddemocontroller.xml): each toggle
// drives the SAME cvar the native panel does and is read back live, so the pill always reflects
// the engine rather than a cached UI value.
//
//   X-Ray                       -> spec_show_xray 0|1
//   True View                   -> cl_demo_predict 0 / 1 / 2
//   Include DOA actions         -> cl_trueview_show_doa_predictions 0|1  (needs True View)
//   Allow demo mismatch versions -> cl_demo_predict 1 <-> 2              (needs True View)
//
// WHY ITS OWN FILE: the Camera Editor bottom bar and the Config panel both expose this popup.
// Two copies of four cvar toggles would drift the moment one side gained a fifth. This fragment
// is spliced into BOTH script bodies (CameraEditorJs.h, ConfigHudJs.h), so there is exactly one
// definition of what "Editor Settings" contains and how it looks.
//
// NOT a standalone script. It is a raw-string chunk concatenated into a host IIFE and needs
// these helpers already in the host's scope:
//
//   $, mk(type,parent), lbl(parent,text,color,size), cmd(consoleCommand),
//   rdInt(cvarName), S (theme tokens: .hint .value .clear)
//
// The host owns its own button; this fragment owns the card. Call it once at build time and keep
// the returned handle:
//
//   var pop = mvmEditorSettingsPopup(ctx, { id: 'UniqueRootId', marginBottom: 120, ... });
//   pop.open() / pop.close(why) / pop.toggle() / pop.isOpen() / pop.refresh()
//   pop.overlay / pop.card   -- for host-side layout and debug probes
//
// The overlay is created as a TOP-LEVEL child of ctx (not of the host's root panel) for the same
// reason the Camera Editor's confirm overlay is: as a child of a z53 root it renders behind the
// Graph (z65) and timeline (z55) roots, and the menu is then covered whenever a bottom editor is
// open. Each host passes a unique `id` and deletes that id in its own Teardown.
//
// ACCENT: Editor Settings deliberately does NOT use the Camera Editor's gold accent, which every
// other selected control there already uses. It carries the theme blue (#80bdffff -- S.freeze in
// the editor, S.accent in Config) so "these are engine/demo playback settings, not camera state"
// reads at a glance. Hosts should paint their own trigger button with the same blue.

R"ESJS(
    function mvmEditorSettingsPopup(ctx, opts) {
      opts = opts || {};
      var ACCENT = opts.accent || '#80bdffff';
      var overlay = $.CreatePanel('Panel', ctx, opts.id || 'MvmEditorSettingsRoot', {});
      overlay.visible = false; overlay.hittest = true;
      overlay.style.width = '100%'; overlay.style.height = '100%';
      overlay.style.zIndex = (opts.zIndex || 221) + '';
      overlay.SetPanelEvent('onactivate', function () { close('bgClick'); });
      var card = mk('Panel', overlay); card.hittest = true;
      card.style.width = '360px'; card.style.horizontalAlign = 'right'; card.style.verticalAlign = 'bottom';
      card.style.marginRight = (opts.marginRight == null ? 16 : opts.marginRight) + 'px';
      card.style.marginBottom = (opts.marginBottom == null ? 16 : opts.marginBottom) + 'px';
      card.style.backgroundColor = 'rgba(14,18,24,0.99)';
      card.style.border = '1px solid ' + ACCENT; card.style.borderRadius = '6px';
      card.style.boxShadow = '#000000ee 0px 0px 26px 5px';
      card.style.flowChildren = 'down';
      card.style.paddingTop = '14px'; card.style.paddingBottom = '16px';
      card.style.paddingLeft = '16px'; card.style.paddingRight = '16px';
      card.SetPanelEvent('onactivate', function () { /* swallow clicks inside */ });
      var title = lbl(card, 'EDITOR SETTINGS', ACCENT, 12);
      title.style.fontWeight = 'bold'; title.style.letterSpacing = '2px'; title.style.marginBottom = '4px';
)ESJS"
R"ESJS(
      // One toggle row: label (+ optional explanation) on the left, a pill switch on the right.
      function settingToggle(label, explain, onClick) {
        var r = mk('Panel', card); r.style.flowChildren = 'right'; r.style.width = '100%';
        r.style.marginTop = '12px'; r.style.verticalAlign = 'middle';
        var col = mk('Panel', r); col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
        col.style.verticalAlign = 'center'; col.style.paddingRight = '12px';
        var t = lbl(col, label, S.value, 14); t.style.fontWeight = 'bold';
        if (explain) { var e = lbl(col, explain, S.hint, 10); e.style.marginTop = '2px'; e.style.whiteSpace = 'normal'; }
        var pill = mk('Panel', r); pill.hittest = true; pill.style.width = '46px'; pill.style.height = '24px';
        pill.style.borderRadius = '12px'; pill.style.backgroundColor = '#00000088'; pill.style.verticalAlign = 'center';
        pill.style.border = '1px solid #ffffff14';
        var knob = mk('Panel', pill); knob.hittest = false; knob.style.width = '18px'; knob.style.height = '18px';
        knob.style.borderRadius = '9px'; knob.style.backgroundColor = '#cfd6deff'; knob.style.verticalAlign = 'center';
        knob.style.marginLeft = '3px'; knob.style.marginRight = '3px';
        pill.SetPanelEvent('onactivate', function () { onClick(); $.Schedule(0, refresh); refresh(); });
        return { row: r, label: t, pill: pill, knob: knob };
      }
      function setToggle(rec, on, enabled) {
        rec.pill.style.backgroundColor = on ? '#2bb24cff' : '#00000088';
        rec.pill.style.border = '1px solid ' + (on ? '#1c8a3aff' : '#ffffff14');
        rec.knob.style.horizontalAlign = on ? 'right' : 'left';
        rec.row.style.opacity = enabled ? '1.0' : '0.4';
        rec.pill.hittest = enabled;
      }
      var xrayTog = settingToggle('X-Ray', 'See players through walls (spec_show_xray).', function () {
        cmd('spec_show_xray ' + (rdInt('spec_show_xray') ? 0 : 1));
      });
      var trueviewTog = settingToggle('True View', 'Render each POV as the player actually saw it.', function () {
        var p = rdInt('cl_demo_predict');
        if (p > 0) cmd('cl_demo_predict 0');
        else cmd('cl_demo_predict ' + (rdInt('cl_demo_predict') >= 2 ? 2 : 1));
      });
      var doaTog = settingToggle('Include DOA actions', 'Predict dead-or-alive actions (needs True View).', function () {
        if (rdInt('cl_demo_predict') <= 0) return;
        cmd('cl_trueview_show_doa_predictions ' + (rdInt('cl_trueview_show_doa_predictions') ? 0 : 1));
      });
      var mismatchTog = settingToggle('Allow demo mismatch versions', 'Apply True View even on a mismatched demo build.', function () {
        var p = rdInt('cl_demo_predict');
        if (p === 1) cmd('cl_demo_predict 2');
        else if (p === 2) cmd('cl_demo_predict 1');
      });
)ESJS"
R"ESJS(
      function refresh() {
        var predict = rdInt('cl_demo_predict');
        var tvOn = predict > 0;
        setToggle(xrayTog, rdInt('spec_show_xray') !== 0, true);
        setToggle(trueviewTog, tvOn, true);
        setToggle(doaTog, rdInt('cl_trueview_show_doa_predictions') !== 0, tvOn);
        setToggle(mismatchTog, predict >= 2, tvOn);
      }
      function dbgOn() { try { return !!(opts.debugOn && opts.debugOn()); } catch (e) { return false; } }
      function dbgTag() { return opts.debugTag || 'editorsettings'; }
      function open() {
        if (opts.beforeOpen) opts.beforeOpen();
        refresh(); overlay.visible = true;
        if (opts.onOpen) opts.onOpen();
        if (dbgOn()) $.Msg('[' + dbgTag() + '] open\n');
      }
      function close(why) {
        // Diagnostic for the "popup won't stay open" report: log WHICH call site closed it and
        // the host's state at that instant, gated on mvm_debug so it is silent otherwise. Only
        // logs when it was actually open (skips no-op closes).
        if (dbgOn() && overlay.visible) {
          var extra = '';
          try { extra = opts.debugState ? (' ' + opts.debugState()) : ''; } catch (e) { extra = ''; }
          $.Msg('[' + dbgTag() + '] close why=' + (why || '?') + extra + '\n');
        }
        overlay.visible = false;
        if (opts.onClose) opts.onClose(why);
      }
      function toggle() { if (overlay.visible) close('reclick'); else open(); }
      return {
        overlay: overlay, card: card, accent: ACCENT,
        open: open, close: close, toggle: toggle, refresh: refresh,
        // The card rises off the gear at the inspector's edge, so it has to move when the
        // operator drags that edge -- otherwise it opens detached from the button that opened it.
        setMarginRight: function (px) { card.style.marginRight = Math.floor(px) + 'px'; },
        isOpen: function () { return !!overlay.visible; }
      };
    }
)ESJS"
