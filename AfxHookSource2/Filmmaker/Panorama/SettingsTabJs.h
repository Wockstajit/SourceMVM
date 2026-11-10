#pragma once

namespace Filmmaker {

// The SOURCE:MVM tab in CS2's own settings menu (pause menu > Settings, and the main menu's
// settings): a radio button added to the native tab row after CROSSHAIR, and a page that takes the
// place of the native tab content while it is selected.
//
// HOW IT SITS IN VALVE'S MENU (reference: layout/settings/settings.xml, scripts/settingsmenu.js)
//
//   #id-navbar-tabs > center container     the native radios, group "SettingsNavBar". Ours joins
//                                          the same group, so picking a native tab unchecks it.
//   #SettingsMenuContent                   holds one panel per native tab. settingsmenu.js keeps
//                                          the active tab in a closure we cannot reach, and does
//                                          nothing when the tab it thinks is active is clicked
//                                          again. So this script never touches those panels: it
//                                          hides the WHOLE container while our radio is checked
//                                          and shows it again when it is not. Coming back to the
//                                          tab that was open before then needs no native call.
//   #MvmSettingsTab                        our page, a sibling of that container.
//
// Rows reuse the native classes (SettingsBackground, SettingsMenuDropdownContainer, PopupButton)
// so they read as Valve's. Layout that those classes do not give a script-built panel is set
// inline, as VideoSettingsResolutionJs.h does.
//
//   C++ -> JS : $.MvmSettingsTab.setState({...})  (Localization/Localization.cpp)
//   JS  -> C++: mirv_filmmaker language <id|auto>, mirv_filmmaker layout <workspace|modular>,
//               mirv_filmmaker keybind capture <action> | cancel | <action> default | reset
inline constexpr const char* kSettingsTabJs = R"MVMTAB(
(function () {
)MVMTAB"
#include "LocalizationJs.h"
R"MVMTAB(
  var priorApi = $.MvmSettingsTab;
  if (priorApi && priorApi.version === 1) { priorApi.tick(); return; }
  if (priorApi) { priorApi.dead = true; priorApi.tick = function () {}; priorApi.setState = function () {}; }

  var api = { version: 1 };
  var state = { language: 'auto', gameLanguage: 'english', layout: 'workspace', languages: [],
                binds: { rows: [], capture: '', conflict: ['', ''] } };
  var builtSig = '';
  // Whether OUR page is the one open. Kept here instead of read from the radio alone: a rebuild
  // of the menu scripts (a language or shortcut change does one) unchecks the radio without the
  // user picking another tab, which threw them back to the Video page (measured in game).
  var selected = false;
  var syncing = false;
  // Counts page builds. A dropdown reports a "pick" when its options are deleted under it, so a
  // dropdown from an earlier build (or from a replaced copy of this script) must never be heard:
  // measured in game, a rebuild switched the language to whichever option was deleted last.
  var buildGen = 0;
  var OPT_LANG = 'mvmlang_', OPT_LAYOUT = 'mvmlayout_';

  function root() {
    var panel = $.GetContextPanel();
    try { while (panel && panel.GetParent()) panel = panel.GetParent(); } catch (e) {}
    return panel;
  }
  function find(id) { try { return root().FindChildTraverse(id); } catch (e) { return null; } }
  function valid(p) { try { return !!p && (!p.IsValid || p.IsValid()); } catch (e) { return false; } }
  function send(command) { try { GameInterfaceAPI.ConsoleCommand(command); } catch (e) {} }
  function languageName(id) {
    for (var i = 0; i < state.languages.length; i++) if (state.languages[i][0] === id) return state.languages[i][1];
    return id;
  }

  function sectionTitle(parent, text) {
    var wrap = $.CreatePanel('Panel', parent, '', { class: 'SettingsSectionTitleContianer' });
    wrap.style.flowChildren = 'right'; wrap.style.marginBottom = '12px';
    var label = mvmLabel(wrap, '', { class: 'SettingsSectionTitleLabel' });
    label.text = text;
    label.style.fontFamily = 'Stratum2, "Arial Unicode MS"'; label.style.fontSize = '28px';
    label.style.fontWeight = 'black'; label.style.fontStyle = 'italic';
    label.style.color = 'rgba(222, 222, 222, 0.4)';
    return label;
  }
  // One native-looking row: the name on the left half, the control on the right half, and one
  // line of plain explanation under the name. The explanation wraps inside its half; the name
  // and the control never do.
  function settingRow(parent, id, name, explain) {
    var row = $.CreatePanel('Panel', parent, id, { class: 'SettingsMenuDropdownContainer' });
    row.style.flowChildren = 'right'; row.style.width = '100%'; row.style.minHeight = '56px';
    // The rule under each native row.
    row.style.borderBottom = '1px solid #ffffff1a';
    var left = $.CreatePanel('Panel', row, '', {});
    left.style.width = '50%'; left.style.flowChildren = 'down'; left.style.verticalAlign = 'center';
    left.style.paddingRight = '24px'; left.style.paddingTop = '8px';
    var title = mvmLabel(left, '', {});
    title.text = name;
    // Font, size and colour come from the native row class, so the name matches Valve's rows.
    title.style.whiteSpace = 'nowrap';
    title.style.textOverflow = 'shrink'; title.style.width = '100%';
    if (explain) {
      var note = mvmLabel(left, '', {});
      note.text = explain;
      note.style.fontSize = '14px'; note.style.color = '#8a9296ff';
      note.style.whiteSpace = 'normal'; note.style.width = '100%';
      note.style.marginTop = '2px'; note.style.marginBottom = '8px';
    }
    var right = $.CreatePanel('Panel', row, '', {});
    right.style.width = '50%'; right.style.verticalAlign = 'center';
    return right;
  }
  function dropDown(parent, id, prefix, options, selected, onPick) {
    var drop = $.CreatePanel('DropDown', parent, id, { class: 'PopupButton White', menuclass: 'DropDownMenu' });
    drop.style.width = '100%'; drop.style.verticalAlign = 'center';
    for (var i = 0; i < options.length; i++) {
      var option = mvmLabel(parent, prefix + options[i][0], { class: 'DropDownMenu' });
      option.text = options[i][1];
      try { drop.AddOption(option); } catch (e) {}
    }
    try { drop.SetSelected(prefix + selected); } catch (e2) {}
    var builtIn = buildGen;
    drop.SetPanelEvent('oninputsubmit', function () {
      if (syncing || api.dead || builtIn !== buildGen || !valid(drop)) return;
      var picked = null;
      try { picked = drop.GetSelected(); } catch (e) {}
      var pickedId = (picked && picked.id) ? ('' + picked.id) : '';
      if (pickedId.indexOf(prefix) === 0) onPick(pickedId.substr(prefix.length));
    });
    return drop;
  }

)MVMTAB"
R"MVMTAB(
  // What each rebindable shortcut does, in the order Input/KeybindsMath.h lists them. The key
  // itself comes from C++ (state.binds), so this is only the wording.
  function shortcutNames() {
    return {
      'marker.place': T('Place a camera key'),
      'marker.delete': T('Delete the camera key you aim at'),
      'marker.edit': T('Select the camera you aim at'),
      'path.play': T('Play or stop the camera path'),
      'demo.pause': T('Pause or resume the demo'),
      'demo.back': T('Skip back 5 seconds'),
      'demo.forward': T('Skip forward 5 seconds'),
      'hud.hide': T('Hide or show the game HUD'),
      'roll.right': T('Roll the free camera right (hold)'),
      'roll.left': T('Roll the free camera left (hold)'),
      'xray': T('X-Ray on or off'),
      'key.ease': T('Easy Ease the selected keyframes'),
      'recoil.test': T('Fire a recoil test shot'),
      'takes.record': T('Record a bot take'),
      'takes.play': T('Play the bot takes'),
      'test.menu': T('Open Config in a practice match')
    };
  }
  function textButton(parent, id, caption, onClick) {
    var button = $.CreatePanel('Button', parent, id, { class: 'PopupButton White' });
    button.style.verticalAlign = 'center';
    var label = mvmLabel(button, '', {});
    label.text = caption;
    label.style.whiteSpace = 'nowrap'; label.style.textOverflow = 'shrink';
    label.style.width = '100%'; label.style.textAlign = 'center';
    button.SetPanelEvent('onactivate', onClick);
    return button;
  }
  function buildShortcuts(scroll) {
    var binds = state.binds || {}, list = binds.rows || [];
    if (!list.length) return;
    var names = shortcutNames();
    var section = $.CreatePanel('Panel', scroll, 'MvmSettingsShortcuts', { class: 'SettingsBackground' });
    section.style.width = '946px'; section.style.horizontalAlign = 'center'; section.style.flowChildren = 'down';
    sectionTitle(section, T('Shortcuts'));
    var rows = $.CreatePanel('Panel', section, '', {});
    rows.style.width = '100%'; rows.style.flowChildren = 'down';
    var conflict = binds.conflict || ['', ''];
    for (var i = 0; i < list.length; i++) {
      (function (id, keyName, isDefault) {
        var capturing = binds.capture === id;
        var problem = '';
        if (conflict[0] === id) {
          problem = conflict[1]
            ? T('That key already has two shortcuts. Move one of them first.')
            : T('That key cannot be used for a shortcut.');
        }
        var right = settingRow(rows, 'MvmBindRow_' + i, names[id] || id, problem);
        right.style.flowChildren = 'right';
        var spacer = $.CreatePanel('Panel', right, '', {});
        spacer.style.width = 'fill-parent-flow(1.0)'; spacer.style.height = '1px';
        if (!isDefault && !capturing) {
          var reset = textButton(right, 'MvmBindReset_' + i, T('Reset'), function () { send('mirv_filmmaker keybind ' + id + ' default'); });
          reset.style.width = '120px'; reset.style.marginRight = '8px';
        }
        var key = textButton(right, 'MvmBindKey_' + i, capturing ? T('Press a key…') : keyName, function () {
          send(capturing ? 'mirv_filmmaker keybind cancel' : ('mirv_filmmaker keybind capture ' + id));
        });
        key.style.width = '200px';
      })(list[i][0], list[i][1], !!list[i][2]);
    }
    var foot = $.CreatePanel('Panel', section, '', {});
    foot.style.width = '100%'; foot.style.flowChildren = 'right'; foot.style.marginTop = '12px'; foot.style.marginBottom = '32px';
    var note = mvmLabel(foot, '', {});
    note.text = T('Click a key, then press the new one. Esc cancels. One key can hold two shortcuts, the way X is both X-Ray and roll.');
    note.style.width = 'fill-parent-flow(1.0)'; note.style.whiteSpace = 'normal';
    note.style.fontSize = '14px'; note.style.color = '#8a9296ff'; note.style.verticalAlign = 'center';
    note.style.paddingRight = '24px';
    var all = textButton(foot, 'MvmBindResetAll', T('Reset all shortcuts'), function () { send('mirv_filmmaker keybind reset'); });
    all.style.width = '260px';
  }

)MVMTAB"
R"MVMTAB(
  // The controls that are not rebindable, listed so people can see how everything is driven.
  // Key names are what is printed on the keyboard and are not translated.
  function buildFixedControls(scroll) {
    var ease = K('key.ease', 'F9');
    var list = [
      [T('Scroll'), T('Switch between first person, chase and free camera')],
      ['Shift + ' + T('Scroll'), T('Free camera speed')],
      ['Ctrl + ' + T('Scroll'), T('Field of view')],
      [T('Hold click'), T('Look around in the chase and free camera')],
      ['W A S D', T('Move the free camera')],
      ['Shift', T('Slow the free camera while held')],
      ['ESC', T('Close the open panel, or cancel what you are doing')],
      ['Ctrl + Z', T('Undo')],
      ['Ctrl + Shift + Z', T('Redo')],
      ['Ctrl + A', T('Select all keyframes')],
      ['Del', T('Delete the selected keyframes or scene object')],
      ['Shift + ' + ease, T('Ease into the selected keyframes')],
      ['Ctrl + Shift + ' + ease, T('Ease out of the selected keyframes')],
      ['Ctrl + C / D / V', T('Copy, duplicate or paste the selected scene object')]
    ];
    var section = $.CreatePanel('Panel', scroll, 'MvmSettingsFixed', { class: 'SettingsBackground' });
    section.style.width = '946px'; section.style.horizontalAlign = 'center'; section.style.flowChildren = 'down';
    section.style.marginBottom = '48px';
    sectionTitle(section, T('Fixed controls'));
    var rows = $.CreatePanel('Panel', section, '', {});
    rows.style.width = '100%'; rows.style.flowChildren = 'down';
    for (var i = 0; i < list.length; i++) {
      var right = settingRow(rows, 'MvmFixedRow_' + i, list[i][1], '');
      var cap = mvmLabel(right, '', {});
      cap.text = list[i][0];
      cap.style.width = '100%'; cap.style.textAlign = 'right'; cap.style.verticalAlign = 'center';
      cap.style.fontFamily = 'Stratum2, "Arial Unicode MS"'; cap.style.fontSize = '20px';
      cap.style.fontWeight = 'bold'; cap.style.color = '#8a9296ff'; cap.style.whiteSpace = 'nowrap';
      cap.style.paddingRight = '16px';
    }
  }

  function buildPage(tab) {
    buildGen++;
    var old = tab.Children();
    for (var c = 0; c < old.length; c++) old[c].DeleteAsync(0);

    // The native tabs start with their own dark strip of jump buttons; an empty one keeps this
    // page's first section at the same height as theirs.
    var strip = $.CreatePanel('Panel', tab, '', { class: 'content-navbar__tabs content-navbar__tabs--dark content-navbar__tabs--noflow' });
    strip.style.width = '100%';
    var scroll = $.CreatePanel('Panel', tab, 'MvmSettingsTabContent', { class: 'SettingsMenuTabContent vscroll' });
    scroll.style.width = '100%'; scroll.style.height = 'fill-parent-flow(1.0)';
    scroll.style.flowChildren = 'down'; scroll.style.overflow = 'squish scroll';
    var section = $.CreatePanel('Panel', scroll, 'MvmSettingsInterface', { class: 'SettingsBackground' });
    section.style.width = '946px'; section.style.horizontalAlign = 'center';
    section.style.flowChildren = 'down';
    sectionTitle(section, T('Interface'));
    var rows = $.CreatePanel('Panel', section, '', {});
    rows.style.width = '100%'; rows.style.flowChildren = 'down';

    var languages = [['auto', T('Automatic ({0})', languageName(state.gameLanguage))]];
    for (var i = 0; i < state.languages.length; i++) languages.push(state.languages[i]);
    dropDown(
      settingRow(rows, 'MvmSettingsLanguageRow', T('Language'),
        T('Automatic follows the language CS2 runs in.')),
      'MvmSettingsLanguage', OPT_LANG, languages, state.language,
      function (id) { if (id !== state.language) send('mirv_filmmaker language ' + id); });

    dropDown(
      settingRow(rows, 'MvmSettingsLayoutRow', T('Window layout'),
        T('Docked shows one panel beside the game. Modular windows are movable and keep the game full screen.')),
      'MvmSettingsLayout', OPT_LAYOUT,
      [['workspace', T('Docked')], ['modular', T('Modular windows')]], state.layout,
      function (id) { if (id !== state.layout) send('mirv_filmmaker layout ' + id); });

    buildShortcuts(scroll);
    buildFixedControls(scroll);
  }

  // Everything the page shows, as one string: a rebuild happens only when it changes, so the
  // open dropdown is not torn down by the periodic state push.
  function signature() {
    return [$.MvmI18n.rev, state.language, state.gameLanguage, state.layout, state.languages.length,
            JSON.stringify(state.binds || {})].join('|');
  }

  function ensure() {
    var lastNative = find('CrosshairRadio'), content = find('SettingsMenuContent');
    if (!valid(lastNative) || !valid(content)) return null;
    var nav = lastNative.GetParent(), host = content.GetParent();
    if (!valid(nav) || !valid(host)) return null;

    var radio = find('MvmSettingsRadio');
    if (!valid(radio)) {
      // A radio created into a live group comes up checked (measured in game). The menu has just
      // opened on a native tab, so hand the selection back to the one that had it.
      var siblings = nav.Children(), wasChecked = null;
      for (var n = 0; n < siblings.length; n++) { try { if (siblings[n].checked) wasChecked = siblings[n]; } catch (e0) {} }
      radio = $.CreatePanel('RadioButton', nav, 'MvmSettingsRadio', { group: 'SettingsNavBar', class: 'content-navbar__tabs__btn' });
      try { if (wasChecked && radio.checked) wasChecked.checked = true; } catch (e1) {}
      // The product name, the same in every language.
      var caption = mvmLabel(radio, '', {});
      caption.text = 'SOURCE:MVM';
      radio.SetPanelEvent('onactivate', function () { selected = true; api.tick(); });
      builtSig = '';
    }
    var tab = find('MvmSettingsTab');
    if (!valid(tab)) {
      tab = $.CreatePanel('Panel', host, 'MvmSettingsTab', { class: 'SettingsMenuTab Active' });
      tab.style.width = '100%'; tab.style.height = '100%'; tab.style.flowChildren = 'down';
      tab.style.opacity = '1.0'; tab.visible = false;
      builtSig = '';
    }
    var sig = signature();
    if (sig !== builtSig) { builtSig = sig; syncing = true; try { buildPage(tab); } catch (e) { $.Msg('[mvmsettings] build failed: ' + e + '\n'); } syncing = false; }

    // A native tab is checked: the user left. None is checked while we are selected: the radio
    // was unchecked behind our back, so put it back.
    var nativeChecked = false, mineChecked = false;
    try {
      var all = nav.Children();
      for (var c = 0; c < all.length; c++) {
        if (!all[c].checked) continue;
        if (all[c].id === 'MvmSettingsRadio') mineChecked = true; else nativeChecked = true;
      }
    } catch (e3) {}
    if (nativeChecked) selected = false;
    else if (mineChecked) selected = true;
    else if (selected) { try { radio.checked = true; } catch (e4) {} }
    if (tab.visible !== selected) tab.visible = selected;
    if (content.visible === selected) content.visible = !selected;
    return tab;
  }

  api.setState = function (next) {
    if (!next) return;
    if (next.language) state.language = next.language;
    if (next.gameLanguage) state.gameLanguage = next.gameLanguage;
    if (next.layout) state.layout = next.layout;
    if (next.languages && next.languages.length) state.languages = next.languages;
    if (next.binds) state.binds = next.binds;
    api.tick();
  };
  api.open = function () {
    var radio = find('MvmSettingsRadio');
    if (valid(radio)) { selected = true; try { radio.checked = true; } catch (e) {} api.tick(); }
  };
  api.tick = function () { try { ensure(); } catch (e) {} };
  $.MvmSettingsTab = api;
  api.tick();
  (function repeat() { api.tick(); try { $.Schedule(0.1, repeat); } catch (e) {} })();
})();
)MVMTAB";

} // namespace Filmmaker
