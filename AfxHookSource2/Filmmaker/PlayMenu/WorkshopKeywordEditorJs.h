#pragma once

// Fragment 3 of the Play-page script: the editor for the "hide training maps" keyword list.
//
// A CENTRED POPUP, not an inline sidebar section. The first version stacked a label and a
// TextEntry per category directly into the play page's left sidebar, which is a narrow fixed
// column -- it stretched the sidebar and pushed the controls that matter out of view. A list of
// words needs a wide field; the sidebar cannot give it one, so the editor overlays the menu
// instead and the sidebar keeps a single "Keywords" button.
//
// One list, one field. See WorkshopOrganizerStore.h for why categories were dropped.
//
// Enter on a TextEntry is 'oninputsubmit', NOT 'onactivate' (which is button activation and
// never fires here); Esc is 'oncancel'.

namespace Filmmaker {

inline const char* kWorkshopKeywordEditorJs = R"FMJS(
(function () {
  try {
    var M = $.MvmPlay;
    if (!M) return;

    var overlay = null;
    var popup = null;
    var entry = null;

    function joined() {
      var w = (M.wsData() || {}).words || [];
      return w.join(', ');
    }

    function commit() {
      var text = '';
      try { text = '' + entry.text; } catch (e) {}
      // The command queue is newline separated, so a pasted newline would split the line in
      // half and the tail would be parsed as its own (unknown) command.
      text = text.replace(/[\r\n]/g, ' ');
      var live = M.wsData();
      if (live) {
        live.words = [];
        var parts = text.split(',');
        for (var p = 0; p < parts.length; ++p) {
          var w = parts[p].replace(/^\s+|\s+$/g, '').toLowerCase();
          if (w) live.words.push(w);
        }
      }
      M.cmd('ws words ' + text);
      if (M.wsApply) M.wsApply();
    }

    function ensurePopup() {
      if (M.overlayAlive(overlay)) return true;
      overlay = null; popup = null; entry = null;

      // Shared overlay plumbing: a click-eating backdrop and top-root parenting, so editing the
      // list cannot also click the map tile behind it. See the note in PlayMenuStripJs.
      overlay = M.makeOverlay('MvmWsKeywordPopup', '640px');
      popup = overlay.panel;

      $.CreatePanel('Label', popup, '', {
        class: 'map-selection__quick-selection-set-title', text: 'TRAINING KEYWORDS'
      });

      var hint = $.CreatePanel('Label', popup, '', {
        text: 'A workshop map is treated as a training map when its NAME or TITLE contains one of these words. Comma separated. Press Enter to save.'
      });
      hint.style.opacity = '0.65';
      hint.style.marginBottom = '12px';
      hint.style.width = '100%';
      hint.style.textOverflow = 'clip';

      entry = $.CreatePanel('TextEntry', popup, 'MvmWsWords', { class: 'workshop-search-textentry' });
      entry.maxchars = 400;
      entry.style.width = '100%';
      entry.text = joined();
      entry.SetPanelEvent('oninputsubmit', commit);
      entry.SetPanelEvent('oncancel', function () { M.showOverlay(overlay, false); });

      var row = $.CreatePanel('Panel', popup, '', {});
      row.style.flowChildren = 'right';
      row.style.marginTop = '12px';

      var saveBtn = $.CreatePanel('Button', row, 'MvmWsWordsSave', { class: 'PopupButton White' });
      $.CreatePanel('Label', saveBtn, '', { text: 'Save' });
      saveBtn.SetPanelEvent('onactivate', commit);

      var resetBtn = $.CreatePanel('Button', row, 'MvmWsWordsReset', { class: 'PopupButton' });
      $.CreatePanel('Label', resetBtn, '', { text: 'Reset to defaults' });
      resetBtn.style.marginLeft = '8px';
      resetBtn.SetPanelEvent('onactivate', function () { M.cmd('ws reset'); });

      var closeBtn = $.CreatePanel('Button', row, 'MvmWsWordsClose', { class: 'PopupButton' });
      $.CreatePanel('Label', closeBtn, '', { text: 'Close' });
      closeBtn.style.marginLeft = '8px';
      closeBtn.SetPanelEvent('onactivate', function () { M.showOverlay(overlay, false); });

      return true;
    }

    M.wsToggleEditor = function () {
      if (!ensurePopup()) return false;
      var show = !M.overlayVisible(overlay);
      if (show) { try { entry.text = joined(); } catch (e) {} }
      return M.showOverlay(overlay, show);
    };

    // Called after every C++ payload push. Only writes when the text actually changed, so a
    // push that lands while the user is mid-edit does not move their caret.
    M.wsSyncEditor = function () {
      if (!entry) return;
      var want = joined();
      try { if (('' + entry.text) !== want) entry.text = want; } catch (e) {}
    };
  } catch (e) {
    try { $.Msg('[mvmplay] keyword editor inject failed: ' + e + '\n'); } catch (e2) {}
  }
})();
)FMJS";

} // namespace Filmmaker
