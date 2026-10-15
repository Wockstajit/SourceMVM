#pragma once

namespace Filmmaker {

// This augments the native settings_video.xml controls instead of creating another settings
// page. Normal display modes still go through the CSGOResolutionSelectionChanged event; only filmmaker
// custom entries use the proven SDL-backed `mirv_filmmaker window size` path.
//
// Styling rule: reuse the native csgostyles classes (PopupPanel/PopupTitle/PopupButton and the
// default TextEntry chrome) so the popup and settings row read as Valve UI. Inline styles are
// for layout only (widths, flow, margins) — never for recreating button/panel chrome by hand.
inline constexpr const char* kVideoSettingsResolutionJs = R"MVMRES(
(function () {
  var priorApi = $.MvmResolutionSettings;
  if (priorApi && priorApi.version === 3) { priorApi.tick(); return; }
  // An older instance may still have a scheduled tick in a live-edited Panorama context.
  // Neutralize that closure before replacing it so it cannot keep re-injecting stale panels.
  if (priorApi) { priorApi.tick = function () {}; priorApi.setPresets = function () {}; }

  var api = {};
  var presets = [];
  var popup = null;
  var fitMode = 'exact';
  var widthField = null, heightField = null, preview = null, errorLabel = null, presetList = null, applyButton = null;
  var activeCustom = null;
  var ratios = [
    ['9:16','Vertical (mobile)',9/16], ['4:5','Portrait (social)',4/5],
    ['1:1','Square',1], ['5:4','5:4',5/4], ['4:3','Full frame',4/3],
    ['1.375:1','Academy',1.375], ['1.43:1','IMAX film',1.43],
    ['3:2','35mm still',3/2], ['16:10','Widescreen',16/10],
    ['1.66:1','European widescreen',5/3], ['16:9','Widescreen HD',16/9],
    ['1.85:1','US widescreen',1.85], ['1.90:1','IMAX digital',1.90],
    ['2:1','Univisium',2], ['2.20:1','70mm',2.20], ['21:9','Ultrawide',21/9],
    ['2.35:1','CinemaScope',2.35], ['2.39:1','Anamorphic scope',2.39],
    ['2.76:1','Ultra Panavision 70',2.76]
  ];

  function root() {
    var panel = $.GetContextPanel();
    try { while (panel && panel.GetParent()) panel = panel.GetParent(); } catch (e) {}
    return panel;
  }
  function find(id) { try { return root().FindChildTraverse(id); } catch (e) { return null; } }
  function send(command) { try { GameInterfaceAPI.ConsoleCommand(command); } catch (e) {} }
  function even(value) { var n = 2 * Math.floor(value / 2 + 0.5); return n < 2 ? 2 : n; }
  function validSize(width, height) { return width >= 640 && height >= 360 && width <= 16384 && height <= 16384; }
  function match(width, height) {
    var measured = width / height, best = ratios[0], distance = 999;
    for (var i = 0; i < ratios.length; i++) {
      var d = Math.abs(Math.log(measured) - Math.log(ratios[i][2]));
      if (d < distance) { distance = d; best = ratios[i]; }
    }
    return { entry: best, error: (measured / best[2] - 1) * 100 };
  }
  function aspectText(width, height, fitted) {
    var m = match(width, height), exact = Math.abs(m.error) <= 0.2;
    var prefix = fitted || exact ? '' : 'Closest ';
    return prefix + m.entry[0] + ' · ' + m.entry[1] + (exact ? '' : ' · ' + (m.error > 0 ? '+' : '') + m.error.toFixed(1) + '%');
  }
  function commandSize(width, height) {
    send('mirv_filmmaker window size ' + width + ' ' + height);
    activeCustom = { width: width, height: height };
    paintAction();
  }
  function persistPresets() {
    send('mirv_filmmaker settings ui video.customResolutions ' + (presets.length ? presets.join(',') : '-'));
  }
  function savePreset(width, height) {
    var value = width + 'x' + height;
    if (presets.indexOf(value) < 0) presets.push(value);
    if (presets.length > 6) presets.shift();
    persistPresets();
  }
  function parsePreset(value) {
    var m = /^(\d{3,5})x(\d{3,5})$/.exec(value || '');
    if (!m) return null;
    var width = parseInt(m[1], 10), height = parseInt(m[2], 10);
    return validSize(width, height) ? { width: width, height: height } : null;
  }
)MVMRES" R"MVMRES(

  // Muted settings-row label look, matching CSGOSettingsEnum Label in settings_enum.css.
  function styleSettingsLabel(label, size) {
    label.style.fontFamily = 'Stratum2, Arial';
    label.style.fontSize = (size || 18) + 'px';
    label.style.fontWeight = 'thin';
    label.style.letterSpacing = '1px';
    label.style.color = '#7d8688ff';
  }

  function paintAction() {
    var status = find('MvmResolutionActionStatus');
    if (!status) return;
    status.text = activeCustom
      ? activeCustom.width + ' × ' + activeCustom.height + ' · ' + aspectText(activeCustom.width, activeCustom.height, false)
      : '';
    status.style.color = '#8bb2d5ff';
    var caption = find('MvmResolutionActionCaption');
    if (caption) caption.text = activeCustom ? 'EDIT CUSTOM…' : 'SET CUSTOM…';
  }
  function ensureActionRow(resolution) {
    var nativeRow = resolution.GetParent(), list = nativeRow && nativeRow.GetParent();
    if (!nativeRow || !list) return;
    var row = find('MvmResolutionActionRow');
    if (!row) {
      row = $.CreatePanel('Panel', list, 'MvmResolutionActionRow', {});
      row.AddClass('SettingsMenuDropdownContainer');
      row.style.flowChildren = 'right';
      var title = $.CreatePanel('Label', row, 'MvmResolutionActionTitle', {});
      title.text = 'Custom Resolution'; title.style.width = '50%'; title.style.verticalAlign = 'middle';
      styleSettingsLabel(title, 18);
      // Match the brightness of the neighboring native row labels, not the muted caption gray.
      title.style.color = '#e8eaecff';
      var controls = $.CreatePanel('Panel', row, 'MvmResolutionActionControls', {});
      controls.style.width = '50%'; controls.style.height = '100%'; controls.style.flowChildren = 'down';
      controls.style.paddingTop = '4px'; controls.style.paddingBottom = '4px';
      var button = makeButton(controls, 'MvmResolutionActionButton', 'SET CUSTOM…', false, openPopup, 'MvmResolutionActionCaption');
      button.style.horizontalAlign = 'right'; button.style.marginRight = '0px';
      button.__label.style.fontSize = '18px';
      button.__label.style.margin = '6px 10px 4px';
      var status = $.CreatePanel('Label', controls, 'MvmResolutionActionStatus', {});
      styleSettingsLabel(status, 12);
      status.style.horizontalAlign = 'right'; status.style.marginRight = '10px';
      status.style.whiteSpace = 'nowrap';
      try { list.MoveChildAfter(row, nativeRow); } catch (e) {}
    }
    paintAction();
  }

  // Native-looking button. The PopupButton class supplies hover wash and UI sounds where
  // csgostyles is live, but every visual property is ALSO set explicitly: the settings context
  // does not resolve every csgostyles variable, and an unresolved color renders as red.
  function setPrimary(button, primary) {
    button.__primary = !!primary;
    button.style.backgroundColor = primary ? '#417ca9ff' : 'rgba(0, 0, 0, 0)';
  }
  function makeButton(parent, id, caption, primary, action, labelId) {
    var button = $.CreatePanel('Button', parent, id || '', {});
    try { button.AddClass('PopupButton'); } catch (e) {}
    button.style.borderRadius = '2px';
    button.style.transitionProperty = 'background-color';
    button.style.transitionDuration = '0.1s';
    setPrimary(button, primary);
    button.SetPanelEvent('onmouseover', function () {
      if (!button.__primary) button.style.backgroundColor = 'rgba(255, 255, 255, 0.05)';
    });
    button.SetPanelEvent('onmouseout', function () {
      button.style.backgroundColor = button.__primary ? '#417ca9ff' : 'rgba(0, 0, 0, 0)';
    });
    var label = $.CreatePanel('Label', button, labelId || '', {});
    label.text = caption;
    label.style.fontFamily = 'Stratum2, Arial';
    label.style.fontSize = '18px';
    label.style.fontWeight = 'medium';
    label.style.color = '#ffffffff';
    label.style.textTransform = 'uppercase';
    label.style.textAlign = 'center';
    label.style.horizontalAlign = 'center';
    label.style.verticalAlign = 'middle';
    label.style.margin = '10px 12px 8px';
    button.__label = label;
    button.SetPanelEvent('onactivate', action);
    return button;
  }
  function closePopup() {
    if (popup) { popup.DeleteAsync(0); popup = null; }
    // These live inside the popup; keeping the references past the delete makes the periodic
    // setPresets refresh throw "Underlying panel is deleted" in renderPresets.
    widthField = null; heightField = null; preview = null; errorLabel = null; presetList = null; applyButton = null;
  }
  function popupValues() {
    var width = parseInt(widthField ? widthField.text : '', 10);
    var height = parseInt(heightField ? heightField.text : '', 10);
    if (!validSize(width, height)) return null;
    width = even(width); height = even(height);
    var fitted = false;
    if (fitMode === 'nearest') {
      var nearest = match(width, height).entry;
      height = even(width / nearest[2]); fitted = true;
    }
    return { width: width, height: height, fitted: fitted };
  }
  function paintPopup() {
    if (!popup) return;
    var value = popupValues();
    if (!value) {
      preview.text = 'Enter at least 640 × 360.'; preview.style.color = '#997a7aff';
      errorLabel.text = 'Width and height must be whole pixels.'; return;
    }
    if (applyButton) applyButton.__label.text = isSaved(value) ? 'Apply' : 'Save & Apply';
    preview.text = value.width + ' × ' + value.height + '  ·  ' + aspectText(value.width, value.height, value.fitted);
    preview.style.color = '#8bb2d5ff'; errorLabel.text = '';
  }
  function isSaved(value) { return presets.indexOf(value.width + 'x' + value.height) >= 0; }
  function applyPopup() {
    var value = popupValues();
    if (!value) { paintPopup(); return; }
    commandSize(value.width, value.height);
    if (!isSaved(value)) savePreset(value.width, value.height);
    closePopup();
  }
  function loadPreset(index) {
    var size = parsePreset(presets[index]);
    if (!size || !widthField || !heightField) return;
    widthField.text = '' + size.width; heightField.text = '' + size.height; paintPopup();
  }
  function removePreset(index) {
    if (index < 0 || index >= presets.length) return;
    presets.splice(index, 1); persistPresets(); renderPresets(); paintPopup();
  }
  function renderPresets() {
    if (!presetList) return;
    try { if (presetList.IsValid && !presetList.IsValid()) { presetList = null; return; } } catch (e) { presetList = null; return; }
    var old = presetList.Children();
    for (var c = 0; c < old.length; c++) old[c].DeleteAsync(0);
    if (!presets.length) {
      var empty = $.CreatePanel('Label', presetList, 'MvmSavedResolutionEmpty', {});
      empty.text = 'No saved resolutions yet.';
      styleSettingsLabel(empty, 14);
      empty.style.marginTop = '2px'; empty.style.marginLeft = '4px';
      return;
    }
    for (var i = 0; i < presets.length; i++) {
      var size = parsePreset(presets[i]); if (!size) continue;
      var row = $.CreatePanel('Panel', presetList, 'MvmSavedResolutionRow' + i, {});
      row.style.width = '100%'; row.style.flowChildren = 'right';
      var load = makeButton(row, 'MvmSavedResolution' + i,
        size.width + ' × ' + size.height + '  ·  ' + aspectText(size.width, size.height, false), false,
        (function (index) { return function () { loadPreset(index); }; })(i));
      load.style.width = 'fill-parent-flow(1.0)'; load.style.marginLeft = '0px';
      load.__label.style.fontSize = '16px'; load.__label.style.horizontalAlign = 'left';
      load.__label.style.textTransform = 'none'; load.__label.style.margin = '8px 10px 6px';
      var remove = makeButton(row, 'MvmSavedResolutionDelete' + i, 'REMOVE', false,
        (function (index) { return function () { removePreset(index); }; })(i));
      remove.style.marginRight = '0px';
      remove.__label.style.fontSize = '14px'; remove.__label.style.margin = '9px 10px 7px';
      remove.__label.style.color = '#7d8688ff';
    }
  }
)MVMRES" R"MVMRES(
  function openPopup() {
    closePopup(); fitMode = 'exact';
    var currentWidth = 1920, currentHeight = 1080;
    if (activeCustom) { currentWidth = activeCustom.width; currentHeight = activeCustom.height; }

    popup = $.CreatePanel('Panel', root(), 'MvmCustomResolutionPopup', {});
    popup.style.width = '100%'; popup.style.height = '100%'; popup.style.zIndex = '900';
    popup.style.backgroundColor = '#000000b0'; popup.hittest = true;
    popup.SetPanelEvent('onactivate', closePopup);
    // The .PopupPanel/.PopupTitle classes live in popups_shared.css, which is NOT loaded in the
    // settings context — their exact values are inlined here instead (verified 2026-08-28: relying
    // on those classes renders an unstyled, scattered popup).
    var card = $.CreatePanel('Panel', popup, 'MvmCustomResolutionCard', {});
    card.style.width = '620px'; card.style.flowChildren = 'down';
    card.style.horizontalAlign = 'center'; card.style.verticalAlign = 'middle';
    card.style.backgroundColor = '#2c2c2cff'; card.style.border = '1px solid #414141ff';
    card.style.paddingBottom = '16px'; card.style.boxShadow = '#000000cc 0px 8px 28px 0px';
    card.hittest = true;
    card.SetPanelEvent('onactivate', function () {});
    var title = $.CreatePanel('Label', card, '', {});
    title.text = 'Custom Resolution';
    title.style.width = '100%'; title.style.backgroundColor = '#00000040';
    title.style.padding = '16px 28px'; title.style.marginBottom = '15px';
    title.style.fontFamily = 'Stratum2, Arial'; title.style.fontSize = '28px';
    title.style.fontWeight = 'bold'; title.style.textTransform = 'uppercase';
    title.style.color = '#979797ff';
    var body = $.CreatePanel('Panel', card, '', {});
    body.style.width = '100%'; body.style.flowChildren = 'down';
    body.style.paddingLeft = '28px'; body.style.paddingRight = '28px';
    var subtitle = $.CreatePanel('Label', body, '', {});
    subtitle.text = 'Set the live CS2 render size for filming.';
    styleSettingsLabel(subtitle, 16);

    var fields = $.CreatePanel('Panel', body, '', {}); fields.style.width = '100%'; fields.style.flowChildren = 'right'; fields.style.marginTop = '20px';
    function makeField(id, caption, value) {
      var col = $.CreatePanel('Panel', fields, '', {}); col.style.width = 'fill-parent-flow(1.0)'; col.style.flowChildren = 'down'; col.style.marginRight = '16px';
      var label = $.CreatePanel('Label', col, '', {}); label.text = caption;
      styleSettingsLabel(label, 13); label.style.marginBottom = '6px';
      var field = $.CreatePanel('TextEntry', col, id, {}); field.text = '' + value;
      field.style.width = '100%'; field.style.height = '40px';
      field.style.padding = '6px 12px'; field.style.fontFamily = 'Stratum2, Arial';
      field.style.fontSize = '18px'; field.style.color = '#dfe3e6ff';
      field.style.backgroundColor = '#00000040';
      field.style.border = '1px solid #4b4b4bff'; field.style.borderRadius = '2px';
      field.SetPanelEvent('ontextchanged', paintPopup); return field;
    }
    widthField = makeField('MvmCustomWidth', 'WIDTH', currentWidth);
    heightField = makeField('MvmCustomHeight', 'HEIGHT', currentHeight);
    heightField.GetParent().style.marginRight = '0px';

    var fitTitle = $.CreatePanel('Label', body, '', {}); fitTitle.text = 'ASPECT FIT';
    styleSettingsLabel(fitTitle, 13); fitTitle.style.marginTop = '20px'; fitTitle.style.marginBottom = '2px';
    var fit = $.CreatePanel('Panel', body, '', {}); fit.style.width = '100%'; fit.style.flowChildren = 'right';
    var exactButton, nearestButton;
    function paintFit() {
      setPrimary(exactButton, fitMode === 'exact');
      setPrimary(nearestButton, fitMode === 'nearest');
      paintPopup();
    }
    exactButton = makeButton(fit, 'MvmFitCustom', 'FIT TO CUSTOM', false, function () { fitMode = 'exact'; paintFit(); });
    exactButton.style.marginLeft = '0px'; exactButton.style.width = 'fill-parent-flow(1.0)';
    exactButton.__label.style.fontSize = '16px';
    nearestButton = makeButton(fit, 'MvmFitNearest', 'FIT TO NEAREST ASPECT', false, function () { fitMode = 'nearest'; paintFit(); });
    nearestButton.style.marginRight = '0px'; nearestButton.style.width = 'fill-parent-flow(1.0)';
    nearestButton.__label.style.fontSize = '16px';

    preview = $.CreatePanel('Label', body, '', {}); preview.style.fontFamily = 'Stratum2, Arial'; preview.style.fontSize = '16px';
    preview.style.marginTop = '16px'; preview.style.marginLeft = '4px'; preview.style.whiteSpace = 'normal';
    errorLabel = $.CreatePanel('Label', body, '', {});
    styleSettingsLabel(errorLabel, 13); errorLabel.style.color = '#997a7aff';
    errorLabel.style.marginTop = '2px'; errorLabel.style.marginLeft = '4px';
    var savedTitle = $.CreatePanel('Label', body, '', {}); savedTitle.text = 'SAVED RESOLUTIONS';
    styleSettingsLabel(savedTitle, 13); savedTitle.style.marginTop = '16px'; savedTitle.style.marginBottom = '2px';
    presetList = $.CreatePanel('Panel', body, 'MvmSavedResolutionList', {});
    presetList.style.width = '100%'; presetList.style.flowChildren = 'down'; renderPresets();
    var actions = $.CreatePanel('Panel', card, '', {});
    actions.style.flowChildren = 'right'; actions.style.horizontalAlign = 'right';
    actions.style.margin = '25px 32px 0px 32px';
    makeButton(actions, 'MvmCustomResolutionCancel', 'Cancel', false, closePopup);
    // One confirm button. Its caption says whether the size will also be saved, so it is decided by
    // the saved list itself (paintPopup) rather than by showing two buttons for the same action.
    applyButton = makeButton(actions, 'MvmCustomResolutionApply', 'Save & Apply', true, applyPopup);
    paintFit(); widthField.SetFocus();
  }

  function removeLegacyOptions(resolution) {
    if (resolution.__mvmLegacyCleaned) return;
    resolution.__mvmLegacyCleaned = true;
    var hadLegacy = false;
    var children = resolution.Children();
    for (var i = 0; i < children.length; i++) {
      var id = children[i].id || '';
      if (id === 'MvmResolutionCustom' || id.indexOf('MvmResolutionPreset') === 0) {
        hadLegacy = true; children[i].DeleteAsync(0);
      }
    }
    var oldStatus = find('MvmResolutionStatusRow');
    if (oldStatus) oldStatus.DeleteAsync(0);
    if (hadLegacy) {
      // v1 registered action panels as real options. Clearing and asking Valve to rebuild is the
      // only reliable way to remove the stale registrations as well as their visible panels.
      try { resolution.RemoveAllOptions(); $.DispatchEvent('CSGOAspectRatioSelectionChanged'); } catch (e) {}
    }
  }
  function hookResolution(resolution) {
    removeLegacyOptions(resolution); ensureActionRow(resolution);
    if (resolution.__mvmActionHooked) return;
    resolution.__mvmActionHooked = true;
    resolution.SetPanelEvent('oninputsubmit', function () {
      // SetPanelEvent REPLACES the XML's oninputsubmit="CSGOResolutionSelectionChanged()", so the
      // native handler has to be re-issued here. In the XML that call is a Panorama EVENT, not a JS
      // function: `typeof CSGOResolutionSelectionChanged` is 'undefined' in this context (measured
      // 2026-10-13), and calling it as a function threw a ReferenceError the catch swallowed -- so
      // picking a normal resolution never reached the engine's pending-settings handler.
      try { $.DispatchEvent('CSGOResolutionSelectionChanged'); } catch (e) {}
      activeCustom = null; paintAction();
    });
  }

  // A live-edited context may still hold pre-v3 panels with the old inline chrome; rebuild them.
  (function dropStalePanels() {
    var oldRow = find('MvmResolutionActionRow'); if (oldRow) oldRow.DeleteAsync(0);
    var oldPopup = find('MvmCustomResolutionPopup'); if (oldPopup) oldPopup.DeleteAsync(0);
  })();

  api.setPresets = function (csv) {
    presets = [];
    var parts = (csv || '').split(',');
    for (var i = 0; i < parts.length; i++) if (parsePreset(parts[i]) && presets.indexOf(parts[i]) < 0) presets.push(parts[i]);
    while (presets.length > 6) presets.shift();
    renderPresets();
    api.tick();
  };
  api.version = 3;
  api.open = openPopup;
  api.tick = function () { var resolution = find('ResolutionEnum'); if (resolution) hookResolution(resolution); };
  $.MvmResolutionSettings = api;
  api.tick();
  (function repeat() { api.tick(); try { $.Schedule(0.35, repeat); } catch (e) {} })();
})();
)MVMRES";

} // namespace Filmmaker
