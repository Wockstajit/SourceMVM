// Native CS2 menu scaffolding. Our UI is extracted unchanged; these panels stand in for
// the engine-owned Play/Watch shell and XML layouts that it normally docks into.
(function (global) {
  'use strict';
  var bundle, fixture, model, current, ctx, menus = {}, commands = [];
  var P = global.PanoramaPanel.prototype;
  var originalLocalize = $.Localize;
  var nativeSetImage = P.SetImage;
  P.SetImage = function (source) {
    nativeSetImage.call(this, source);
    for (var ancestor = this; ancestor; ancestor = ancestor.GetParent()) {
      var tint = ancestor.BHasClass('sb-tint--CT') ? '#a5cbe2' : ancestor.BHasClass('sb-tint--TERRORIST') ? '#dfc976' : ancestor.BHasClass('mi-icons__eliminations--bottom') ? '#632c26' : null;
      if (tint) { this.style.washColor = tint; break; }
    }
  };
  var addNativeClass = P.AddClass;
  P.AddClass = function (name) {
    addNativeClass.call(this, name);
    var tint = name === 'sb-tint--CT' ? '#a5cbe2' : name === 'sb-tint--TERRORIST' ? '#dfc976' : null;
    if (tint) {
      function wash(panel) { if (panel.type === 'Image') panel.style.washColor = tint; panel.Children().forEach(wash); }
      wash(this);
    }
  };
  var words = {
    CSGO_Watch_Info_1: 'Duration', CSGO_Watch_Info_2: 'Date', CSGO_Watch_Info_3: 'Map',
    teamname_CT: 'COUNTER-\nTERRORISTS', teamname_TERRORIST: 'TERRORISTS', WatchMenu_Watch: 'WATCH',
    CSGO_Watch_Info_download: 'DOWNLOAD', SFUI_Match_Tied: 'Tied', SFUI_Match_Victory: 'Victory',
    SFUI_Match_Loss: 'Loss', Scoreboard_versus: '-', WatchMenu_Info_Download_Failed: 'Download failed',
    MatchInfo_RoundDataTitle: "{s:playerNameTitle}'s Round Performance",
    WatchMenu_Downloading: 'Downloading', CSGO_Watch_Scoreboard_K: 'K', CSGO_Watch_Scoreboard_A: 'A',
    CSGO_Watch_Scoreboard_D: 'D', CSGO_Watch_Scoreboard_MVP: 'MVP', CSGO_Watch_Scoreboard_Score: 'Score'
  };
  $.Localize = function (token) {
    var key = String(token).replace(/^#/, '');
    if (key.indexOf('SFUI_Map_') === 0) {
      var name = key.slice(9).replace(/^(de_|cs_|ar_)/, '').replace(/_/g, ' ');
      return name.charAt(0).toUpperCase() + name.slice(1);
    }
    return words[key] || originalLocalize(token);
  };
  $.ConstructString = function (token, args) {
    return token === '#CSGO_Watch_Minute:f' ? args.value + ' Minutes' : $.Localize(token);
  };
  Object.assign(words, { Scoreboard_kills_header: 'K', Scoreboard_assists_header: 'A', Scoreboard_deaths_header: 'D',
    Scoreboard_mvps_header: '★', Scoreboard_score_header: 'Score', WatchMenu_Outcome_Won: 'Victory',
    WatchMenu_Outcome_Lost: 'Loss', WatchMenu_Outcome_Tied: 'Tied' });
  P.AddOption = function (option) {
    if (!this.selectEl) {
      var select = document.createElement('select'), self = this;
      select.className = 'native-dropdown'; this.el.appendChild(select); this.selectEl = select;
      select.addEventListener('change', function () { var h = self._events.oninputsubmit; if (h) h(); });
    }
    var opt = document.createElement('option'); opt.value = option.id; opt.textContent = option.text;
    this.selectEl.appendChild(opt); option.visible = false;
  };
  P.SetSelected = function (id) { if (this.selectEl) this.selectEl.value = id; };
  P.GetSelected = function () { return this.selectEl ? this.Children().find(function (p) { return p.id === this.selectEl.value; }, this) : null; };
  var originalWire = P._wireEvent;
  P._wireEvent = function (name) {
    if (name === 'onactivate' && !this.__nativeKeyboard && this.el.closest('.c-native-menu')) {
      this.__nativeKeyboard = true; this.el.tabIndex = 0; this.el.setAttribute('role', 'button');
      var target = this;
      this.el.addEventListener('keydown', function (event) {
        if ((event.key === 'Enter' || event.key === ' ') && event.target === target.el && target.enabled) { event.preventDefault(); target.el.click(); }
      });
    }
    if (name === 'onactivate' && this.type === 'ToggleButton' && !this.__nativeToggle && this.el.closest('.c-native-menu')) {
      this.__nativeToggle = true;
      var self = this; this.el.addEventListener('click', function () { if (self.enabled) self.checked = !self.checked; });
    }
    originalWire.call(this, name);
  };
  function mk(type, parent, id, classes, text) {
    return $.CreatePanel(type, parent, id || '', { class: classes || '', text: text || '' });
  }
  function button(parent, id, classes, text, action) {
    var p = mk('Button', parent, id, classes, text);
    if (classes === 'native-go') { p.style.backgroundColor = '#006900'; p.style.backgroundImgOpacity = '0.18'; }
    p.SetPanelEvent('onactivate', action); return p;
  }
  function image(parent, id, classes, src) {
    var p = mk('Image', parent, id, classes); p.SetImage(src); return p;
  }
  function icon(name) { return 's2r://panorama/images/icons/ui/' + name + '.vsvg'; }
  function applyXml(panel, node, merge) {
    if (!merge) panel = mk(node.tagName, panel, node.getAttribute('id'), node.getAttribute('class'));
    else if (node.getAttribute('class')) node.getAttribute('class').split(/\s+/).forEach(function (c) { panel.AddClass(c); });
    if (node.tagName === 'TextButton' && !panel.textEl) {
      panel.textEl = document.createElement('span'); panel.textEl.className = 'pnl-text'; panel.el.appendChild(panel.textEl);
    }
    Array.from(node.attributes).forEach(function (a) {
      if (a.name === 'text') panel.text = a.value.charAt(0) === '#' ? $.Localize(a.value) : a.value;
      else if (a.name === 'src' && panel.type === 'Image') panel.SetImage(a.value);
      else if (a.name === 'style') a.value.split(';').forEach(function (declaration) {
        var colon = declaration.indexOf(':'); if (colon < 0) return;
        var prop = declaration.slice(0, colon).trim().replace(/-([a-z])/g, function (_, c) { return c.toUpperCase(); });
        panel.style[prop] = declaration.slice(colon + 1).trim();
      });
      else if (a.name === 'hittest') panel.hittest = a.value !== 'false';
      else if (a.name !== 'class' && a.name !== 'id') panel.SetAttributeString(a.name, a.value);
    });
    Array.from(node.children).forEach(function (child) { applyXml(panel, child, false); });
    // Native XML buttons draw their child Label; Button.text is not a second label.
    if (node.tagName === 'Button' && node.children.length && panel.textEl) { panel.textEl.remove(); panel.textEl = null; }
    return panel;
  }
  P.BLoadLayout = function (url) {
    if (!bundle) return false;
    var name = url.replace(/^.*\/layout\//, '').replace(/\.xml$/, '');
    var xml = bundle.layouts[name]; if (!xml) throw new Error('Missing native layout: ' + name);
    var doc = new DOMParser().parseFromString(xml, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('Invalid native XML: ' + name);
    var root = Array.from(doc.documentElement.children).find(function (n) { return !/^(styles|scripts|snippets)$/.test(n.tagName); });
    Array.from(doc.querySelectorAll('snippet')).forEach(function (n) { menus[n.getAttribute('name')] = n.firstElementChild; });
    this.RemoveAndDeleteChildren(); applyXml(this, root, true); return true;
  };
  P.BLoadLayoutSnippet = function (name) {
    if (!menus[name]) return false;
    applyXml(this, menus[name], true); return true;
  };
  // Panorama dialog variables inherit through the subtree (including names containing '-').
  var renderOriginalText = P._renderText;
  P._renderText = function () {
    if (!this.el.closest('.c-native-menu')) return renderOriginalText.call(this);
    if (this.inputEl && this.type === 'TextEntry') this.inputEl.value = String(this._text || '');
    if (!this.textEl) return;
    var panel = this;
    var text = String(this._text || '').replace(/\{[sdif]:([^}]+)\}/g, function (raw, name) {
      for (var p = panel; p; p = p.GetParent()) if (name in p._dialogVars) return String(p._dialogVars[name]);
      return raw;
    });
    this.textEl.textContent = text;
  };
  var setVariable = P.SetDialogVariable;
  P.SetDialogVariable = function (name, value) {
    setVariable.call(this, name, value);
    if (!this.el.closest('.c-native-menu')) return;
    function update(p) { p._renderText(); p.Children().forEach(update); } update(this);
  };
  P.SetDialogVariableInt = P.SetDialogVariable;
  P.SetDialogVariableFloat = P.SetDialogVariable;
  P.FindChildInLayoutFile = P.FindChildTraverse;
  P.SetReadyForDisplay = function (ready) { this._readyForDisplay = !!ready; };
  P.MoveChildBefore = function (child, sibling) {
    if (child === sibling || child.parent !== this || sibling.parent !== this) return;
    this.children.splice(this.children.indexOf(child), 1);
    this.children.splice(this.children.indexOf(sibling), 0, child);
    this.el.insertBefore(child.el, sibling.el);
  };
  P.MoveChildAfter = function (child, sibling) {
    if (child === sibling || child.parent !== this || sibling.parent !== this) return;
    this.children.splice(this.children.indexOf(child), 1);
    this.children.splice(this.children.indexOf(sibling) + 1, 0, child);
    this.el.insertBefore(child.el, sibling.el.nextSibling);
  };
  P.SetParent = function (parent) { this._attach(parent); };
  P.SetAttributeInt = function (key, value) { this.SetAttributeString(key, String(value)); };
  P.GetAttributeInt = function (key, fallback) { return Number(this.GetAttributeString(key, String(fallback))); };
  P.SetSteamID = function (id) {
    this.SetAttributeString('steamid', String(id));
    if (model && model.artwork && model.artwork.avatars) {
      var source = model.artwork.avatars[String(id)];
      if (source) this.SetImage(source);
    }
  };
  P.PopulateFromSteamID = P.SetSteamID;
  // Portraits are local fixture artwork; no Steam identity lookup is made by the mockup.
  P.SetXUID = P.SetSteamID;
  P.TriggerClass = function (name) { this.AddClass(name); };
  var oldTooltip = UiToolkitAPI.ShowTextTooltipOnPanel, oldHideTooltip = UiToolkitAPI.HideTextTooltip;
  var tooltip;
  UiToolkitAPI.HideTextTooltip = function () { if (tooltip) { tooltip.remove(); tooltip = null; } oldHideTooltip(); };
  UiToolkitAPI.ShowTextTooltipOnPanel = function (panel, text) {
    if (!panel.el.closest('.c-native-menu')) return oldTooltip(panel, text);
    UiToolkitAPI.HideTextTooltip();
    if (!text) return;
    tooltip = document.createElement('div'); tooltip.className = 'native-menu-tooltip'; tooltip.setAttribute('role', 'tooltip');
    // Workshop descriptions are content, never script: retain only text, line breaks and font colors.
    var parsed = new DOMParser().parseFromString(String(text), 'text/html');
    function copy(node, parent) {
      if (node.nodeType === 3) { parent.appendChild(document.createTextNode(node.textContent)); return; }
      if (node.nodeType !== 1 || /^(SCRIPT|STYLE|IFRAME|OBJECT)$/.test(node.tagName)) return;
      if (node.tagName === 'BR') { parent.appendChild(document.createElement('br')); return; }
      var next = parent;
      if (node.tagName === 'FONT') { next = document.createElement('span'); var color = node.getAttribute('color') || ''; if (/^(#[a-f0-9]{3,8}|[a-z]+)$/i.test(color)) next.style.color = color; parent.appendChild(next); }
      Array.from(node.childNodes).forEach(function (child) { copy(child, next); });
    }
    Array.from(parsed.body.childNodes).forEach(function (node) { copy(node, tooltip); });
    var root = panel.el.closest('.c-native-menu'); root.appendChild(tooltip);
    var tile = panel.el.getBoundingClientRect(), frame = root.getBoundingClientRect(), scale = frame.height / 1080;
    tooltip.style.left = Math.max(16, Math.min((tile.left - frame.left) / scale - 90, root.clientWidth - 376)) + 'px';
    tooltip.style.top = Math.min((tile.bottom - frame.top) / scale + 16, 1080 - tooltip.offsetHeight - 16) + 'px';
  };
  function switchWatch(name) {
    ['JsYourMatches', 'JsDownloaded'].forEach(function (id) {
      var p = ctx.FindChildTraverse(id); if (p) { p.SetHasClass('WatchMenu--Hide', id !== name); p.visible = id === name; }
    });
    if (name === 'JsDownloaded' && $.Filmmaker) $.Filmmaker.gotoTab('downloaded');
  }
  function setMode(mode) {
    if (mode === current) return;
    global.MvmMockup.context(mode === 'workshop' ? 'menu-workshop' : 'menu-demos');
  }
  function shell(root, mode) {
    UiToolkitAPI.HideTextTooltip();
    ctx = root; current = mode;
    root.AddClass('native-menu'); root.AddClass(root.actuallayoutwidth <= 1440 ? 'AspectRatio4x3' : root.actuallayoutwidth <= 1728 ? 'AspectRatio16x10' : 'AspectRatio16x9');
    if (model.artwork && model.artwork.background) {
      var background = mk('Panel', root, '', 'native-scene');
      background.style.backgroundImage = 'url("' + model.artwork.background + '")';
      root.AddClass('native-scene-captured');
    }
    var menu = mk('Panel', root, 'JsMainMenuContent', 'native-menu-content');
    var top = mk('Panel', root, 'MainMenuNavBar', 'native-top');
    top.style.backgroundImage = 'url("file://{resources}/videos/linemap.webm")';
    top.style.backgroundImgOpacity = '0.24';
    var tools = mk('Panel', top, '', 'native-tools');
    button(tools, '', 'native-icon', '', function () { setMode('downloaded'); }).style.backgroundImage = 'url("' + icon('home') + '")';
    var reel = image(button(tools, 'MainMenuNavBarWatch', 'native-icon native-film', '', function () { setMode('downloaded'); }), '', '', icon('film'));
    if (mode === 'downloaded') reel.style.washColor = '#75d3f5';
    var settings = button(tools, '', 'native-icon', '', function () { notify('Settings preview is available from the mockup sidebar.'); });
    image(settings, '', '', icon('settings')); mk('Label', settings, '', 'native-new', 'NEW!');
    image(button(tools, '', 'native-icon', '', function () { notify('Exit is simulated in this preview.'); }), '', '', icon('power'));
    var tabs = mk('Panel', top, '', 'native-primary-tabs');
    ['INVENTORY', 'LOADOUT', 'PLAY', 'STORE', 'NEWS'].forEach(function (name) {
      button(tabs, '', name === 'PLAY' ? 'native-main-tab active' : 'native-main-tab', name,
        function () { if (name === 'PLAY') setMode('workshop'); else notify(name + ' is outside this menu preview.'); });
    });
    var friends = mk('Panel', root, 'MainMenuFriends', 'native-friends');
    var invite = mk('Panel', friends, '', 'native-invite');
    mk('Label', invite, '', '', 'Invite from');
    mk('Label', invite, '', '', 'Deathmatch\nDefusal Group');
    image(invite, '', 'native-avatar', model.artwork && model.artwork.invite || 'assets/panorama/images/icons/ui/friends.svg');
    image(invite, '', 'native-check', icon('check'));
    image(friends, '', 'native-friend-count', icon('player')); mk('Label', friends, '', 'native-count', '23');
    (model.artwork && model.artwork.friends || Array(12).fill('assets/panorama/images/icons/ui/friends.svg')).forEach(function (source) {
      image(button(friends, '', 'native-friend', '', function () { notify('Friend activity is fixture data.'); }), '', '', source);
    });
    var utilities = mk('Panel', friends, '', 'native-friend-utilities');
    ['broadcast', 'recent', 'invite'].forEach(function (name) { image(button(utilities, '', 'native-friend-utility', '', function () { notify('Friend activity is fixture data.'); }), '', '', icon(name)); });
    var play = mk('Panel', menu, 'JsPlayContent', 'native-page'); play.visible = mode === 'workshop';
    var nav = mk('Panel', play, 'PlayTypeTopNav', 'native-subnav');
    var center = mk('Panel', nav, '', 'native-subtabs');
    button(center, 'Play-listen', 'native-subtab', 'PRACTICE', function () { notify('Select Workshop Maps to preview installed maps.'); });
    button(center, 'Play-workshop', 'native-subtab active', 'WORKSHOP MAPS', function () {});
    buildWorkshop(play);
    var watch = mk('Panel', menu, 'JsWatchContent', 'native-page'); watch.visible = mode !== 'workshop';
    var watchNav = mk('Panel', watch, 'watch-navbar', 'native-subnav');
    var watchTabs = mk('Panel', watchNav, '', 'native-subtabs');
    button(watchTabs, 'WatchNavBarButtonYourMatches', 'native-subtab', 'YOUR MATCHES', function () { switchWatch('JsYourMatches'); });
    button(watchTabs, 'WatchNavBarButtonDownloaded', 'native-subtab active', 'DOWNLOADED', function () { switchWatch('JsDownloaded'); });
    mk('Panel', watchNav, '', 'native-nav-actions');
    var bodies = mk('Panel', watch, '', 'native-watch-bodies');
    var downloaded = mk('Panel', bodies, 'JsDownloaded', 'WatchMenu');
    var your = mk('Panel', bodies, 'JsYourMatches', 'WatchMenu WatchMenu--Hide'); your.visible = false;
    mk('Label', your, '', 'native-empty', 'Your Matches uses Steam matchmaking data. Use Downloaded for local demos.');
    global.MainMenu = { NavigateToTab: function () { setMode('downloaded'); } };
    global.MyPersonaAPI = { GetXuid: function () { return model.persona ? model.persona.xuid : '76561197960265728'; } };
    global.FriendsListAPI = { GetFriendName: function () { return 'Player'; } };
    return menu;
  }
  function buildWorkshop(parent) {
    var body = mk('Panel', parent, '', 'native-workshop-body');
    var sidebar = mk('Panel', body, 'WorkshopSidebar', 'native-workshop-sidebar');
    var search = mk('Panel', sidebar, 'WorkshopSearchBar', 'native-workshop-search');
    var entry = mk('TextEntry', search, 'WorkshopSearchTextEntry', ''); entry.inputEl.placeholder = 'Filter Maps';
    entry.SetPanelEvent('ontextentrychange', function () { if ($.MvmPlay) $.MvmPlay.wsApply(); });
    button(search, 'WorkshopSearchTextEntryCanel', 'native-search-clear', '', function () { entry.text = ''; if ($.MvmPlay) $.MvmPlay.wsApply(); }).style.backgroundImage = 'url("' + icon('close') + '")';
    mk('Label', sidebar, '', 'native-options-label', 'Options');
    var party = mk('ToggleButton', sidebar, 'OpenParty', 'native-party'); mk('Label', party, '', '', 'Open Party'); party.checked = true;
    mk('Panel', party, '', 'native-party-switch');
    party.SetPanelEvent('onactivate', function () { notify('Open Party: ' + (party.checked ? 'on' : 'off')); });
    var visit = button(sidebar, 'WorkshopVisitButton', 'native-workshop-visit', 'Visit Workshop', function () { notify('Workshop browsing opens Steam in game; the preview uses installed map fixtures.'); });
    image(visit, '', '', icon('link'));
    var grid = mk('Panel', body, 'gameModeButtonContainer_workshop', 'native-workshop-grid');
    model.maps.forEach(function (map, i) {
      var p = mk('RadioButton', grid, 'gameModeButtonContainer_workshop_' + i, 'native-map');
      p.SetAttributeString('addon', String(map.workshop_id));
      p.SetAttributeString('mapname', '@workshop/' + map.workshop_id + '/' + map.map);
      var metadata = model.tiles.find(function (t) { return String(t.id) === String(map.workshop_id); });
      p.SetAttributeString('data-tooltip', metadata ? metadata.description : map.description || '');
      p.SetAttributeString('data-workshop-modes', metadata ? metadata.modes : 'casual,competitive,custom');
      var plate = mk('Panel', p, '', 'native-map-plate'); plate.style.backgroundImage = 'url("' + map.imageUrl + '")';
      mk('Label', p, 'MapGroupName', 'native-map-name', map.name);
      p.SetPanelEvent('onactivate', function () {
        grid.Children().forEach(function (t) { t.checked = t === p; });
        global.MvmNativeMenu.selectedMap = map;
      });
      p.SetPanelEvent('onmouseover', function () { UiToolkitAPI.ShowTextTooltipOnPanel(p, p.GetAttributeString('data-tooltip', '')); });
      p.SetPanelEvent('onmouseout', function () { UiToolkitAPI.HideTextTooltip(); });
    });
    var footer = mk('Panel', parent, '', 'native-play-footer');
    var mission = button(footer, 'WorkshopMission', 'native-mission', '', function () { notify('Mission activity is fixture data.'); });
    mission.style.backgroundImage = 'url("assets/panorama/images/map_icons/screenshots/360p/de_vertigo_png.png")';
    mk('Label', mission, '', 'native-mission-title', 'COMPETITIVE on VERTIGO');
    mk('Label', mission, '', 'native-mission-detail', 'Win rounds');
    var go = button(footer, 'WorkshopGo', 'native-go', 'GO', function () {
      if (!global.MvmNativeMenu.selectedMap) { notify('Select a map first.'); return; }
      var map = global.MvmNativeMenu.selectedMap;
      var tile = model.tiles.find(function (tile) { return String(tile.id) === String(map.workshop_id); });
      workshopModePopup(map, tile && tile.modes || 'casual,competitive,custom');
    });
    go.style.backgroundImage = 'url("file://{resources}/videos/linemap.webm")';
  }
  function notify(message) {
    document.getElementById('menu-preview-status').textContent = message;
  }
  function workshopModePopup(map, available) {
    var old = ctx.FindChildTraverse('WorkshopModePopup'); if (old) old.DeleteAsync(0);
    var dim = mk('Panel', ctx, 'WorkshopModePopup', 'native-popup-dim');
    var dialog = mk('Panel', dim, '', 'native-workshop-mode-dialog');
    dialog.BLoadLayout('file://{resources}/layout/popups/popup_workshop_mode_select.xml');
    dialog.RemoveClass('Hidden');
    var title = dialog.Children().find(function (p) { return p.BHasClass('PopupTitle'); });
    if (title) title.text = 'Select a game mode';
    var choices = { casual: 'Casual', competitive: 'Competitive', scrimcomp2v2: 'Wingman', deathmatch: 'Deathmatch', gungameprogressive: 'Arms Race', retakes: 'Retakes', flyingscoutsman: 'Flying Scoutsman', custom: 'Custom' };
    var selected, items = dialog.FindChildTraverse('popup-workshop-mode-items');
    available.split(',').filter(function (mode) { return !!choices[mode]; }).forEach(function (mode) {
      var row = button(items, 'WorkshopMode-' + mode, 'PopupButton native-mode-choice', '', function () {
        selected = mode; items.Children().forEach(function (item) { item.checked = item === row; });
      });
      mk('Label', row, '', '', choices[mode]); if (!selected) { selected = mode; row.checked = true; }
    });
    var go = dialog.FindChildTraverse('GoButton'), cancel = dialog.FindChildTraverse('CancelButton');
    go.text = 'GO'; cancel.text = 'CANCEL';
    go.SetPanelEvent('onactivate', function () { notify('Preview launch: ' + map.name + ' / ' + choices[selected]); dim.DeleteAsync(0); });
    cancel.SetPanelEvent('onactivate', function () { dim.DeleteAsync(0); });
    dim.el.addEventListener('keydown', function (event) { if (event.key === 'Escape') dim.DeleteAsync(0); });
    cancel.el.focus();
  }
  function contextMenu(panel, items) {
    var old = document.getElementById('native-context-menu'); if (old) old.remove();
    var menu = document.createElement('div'); menu.id = 'native-context-menu'; menu.className = 'native-context-menu';
    items.forEach(function (item) {
      var b = document.createElement('button'); b.textContent = item.label;
      b.onclick = function () { menu.remove(); if (item.jsCallback) item.jsCallback(); }; menu.appendChild(b);
    });
    var r = panel.el.getBoundingClientRect(); menu.style.left = Math.min(r.left, innerWidth - 240) + 'px';
    menu.style.top = Math.min(r.bottom, innerHeight - items.length * 36 - 10) + 'px'; document.body.appendChild(menu);
    setTimeout(function () { document.addEventListener('pointerdown', function dismiss(e) {
      if (!menu.contains(e.target)) { menu.remove(); document.removeEventListener('pointerdown', dismiss); }
    }); }, 0); return menu;
  }
  UiToolkitAPI.ShowSimpleContextMenu = function (id, layout, items) {
    contextMenu(ctx.FindChildTraverse(id) || ctx, items);
  };
  var originalConfirm = UiToolkitAPI.ShowGenericPopupOkCancel;
  UiToolkitAPI.ShowGenericPopupOkCancel = function (title, message, classes, okay, cancel) {
    if (!ctx || !ctx.BHasClass('native-menu')) return originalConfirm(title, message, classes, okay, cancel);
    var dim = mk('Panel', ctx, 'NativeMenuConfirm', 'native-popup-dim');
    var dialog = mk('Panel', dim, '', 'native-workshop-mode-dialog');
    mk('Label', dialog, '', 'PopupTitle', title);
    mk('Label', dialog, '', 'native-popup-message', message);
    var row = mk('Panel', dialog, '', 'PopupButtonRow');
    function close(callback) { dim.DeleteAsync(0); if (typeof callback === 'function') callback(); }
    var yes = button(row, 'NativeMenuConfirmOkay', 'PopupButton', '', function () { close(okay); }); mk('Label', yes, '', '', 'OK');
    var no = button(row, 'NativeMenuConfirmCancel', 'PopupButton', '', function () { close(cancel); }); mk('Label', no, '', '', 'CANCEL');
    dim.el.addEventListener('keydown', function (event) { if (event.key === 'Escape') close(cancel); }); no.el.focus();
  };
  function publish() {
    var host = ctx.FindChildTraverse('FilmmakerMenuTab');
    if (host && $.Filmmaker) {
      host.SetAttributeString('demos', JSON.stringify(model.demos));
      host.SetAttributeString('org', JSON.stringify(model.organizer || {}));
      $.Filmmaker.render(); $.Filmmaker.renderOrg();
    }
    var ws = ctx.FindChildTraverse('MvmPlayMenuHost');
    if (ws && $.MvmPlay) { ws.SetAttributeString('ws', JSON.stringify(model.workshop)); $.MvmPlay.renderWs(); }
  }
  function handle(line) {
    commands.push(line); notify('Simulated: ' + line);
    var m;
    if ((m = /^ws sort (\w+)/.exec(line))) model.workshop.sort = m[1];
    else if ((m = /^ws hide (\d)/.exec(line))) model.workshop.hideTraining = m[1] === '1';
    else if ((m = /^ws words (.*)/.exec(line))) model.workshop.words = m[1].split(',').map(function (s) { return s.trim().toLowerCase(); }).filter(Boolean);
    else if (/^ws reset/.test(line)) model.workshop.words = fixture.workshop.words.slice();
    else if ((m = /^renamedemo (\d+) (.+)/.exec(line))) {
      var d = model.demos.demos.find(function (d) { return d.index === Number(m[1]); });
      if (d) {
        var oldKey = (d.folder.replace(/\//g, '\\') + '\\' + d.fileName).toLowerCase();
        d.fileName = m[2] + '.dem';
        var newKey = (d.folder.replace(/\//g, '\\') + '\\' + d.fileName).toLowerCase();
        if (model.organizer.demos && model.organizer.demos[oldKey]) { model.organizer.demos[newKey] = model.organizer.demos[oldKey]; delete model.organizer.demos[oldKey]; }
      }
    } else if ((m = /^hide (\d+)/.exec(line))) model.demos.demos = model.demos.demos.filter(function (d) { return d.index !== Number(m[1]); });
    else if (line === 'clearall') model.demos.demos = [];
    else if ((m = /^org sort (\w+)/.exec(line))) model.organizer.sort = m[1];
    else if ((m = /^org newfolder (.+)/.exec(line))) {
      model.organizer.groups = model.organizer.groups || [];
      model.organizer.groups.push({ id: Date.now(), name: m[1], collapsed: false });
    } else if ((m = /^org (collapse|rename|delfolder) (-?\d+)(?: (.*))?/.exec(line))) {
      var folders = model.organizer.groups || [], f = folders.find(function (f) { return String(f.id) === m[2]; });
      if (m[1] === 'collapse' && m[2] === '-1') model.organizer.rootCollapsed = m[3] === '1';
      if (f && m[1] === 'collapse') f.collapsed = m[3] === '1';
      if (f && m[1] === 'rename') f.name = m[3];
      if (m[1] === 'delfolder') {
        model.organizer.groups = folders.filter(function (f) { return String(f.id) !== m[2]; });
        Object.values(model.organizer.demos || {}).forEach(function (record) { if (record.g === Number(m[2])) record.g = -1; });
      }
    } else if ((m = /^org assign (-?\d+) (.*)/.exec(line))) {
      model.organizer.demos = model.organizer.demos || {};
      model.organizer.demos[m[2]] = Object.assign({}, model.organizer.demos[m[2]], { g: Number(m[1]) });
    } else if (line === 'adddemo' || line === 'addfolder') {
      var next = JSON.parse(JSON.stringify(fixture.demos.demos[1] || fixture.demos.demos[0]));
      if (next) {
        next.index = Math.max(-1, ...model.demos.demos.map(function (d) { return d.index; })) + 1;
        next.fileName = 'preview-added-' + next.index + '.dem'; model.demos.demos.push(next);
      }
    } else if ((m = /^watch (\d+)/.exec(line))) {
      var watched = model.demos.demos.find(function (d) { return d.index === Number(m[1]); });
      if (watched) {
        var key = (watched.folder.replace(/\//g, '\\') + '\\' + watched.fileName).toLowerCase();
        model.organizer.demos[key] = Object.assign({}, model.organizer.demos[key], { played: Math.floor(Date.now() / 1000) });
      }
    }
    publish();
  }
  global.MvmNativeMenu = {
    selectedMap: null,
    ready: async function () {
      var data = await Promise.all([fetch('generated/native-menu.json').then(function (r) { return r.json(); }),
        fetch('generated/menu-fixture.json').then(function (r) { return r.ok ? r.json() : fetch('fixtures/menu-default.json').then(function (r) { if (!r.ok) throw new Error('Missing default menu fixture'); return r.json(); }); })]);
      bundle = data[0]; fixture = data[1]; model = JSON.parse(JSON.stringify(fixture));
      model.organizer = model.organizer || {}; model.workshop = model.workshop || { sort: 'name', words: [], items: {} };
      model.organizer.demos = model.organizer.demos || {};
      var doc = new DOMParser().parseFromString(bundle.layouts.mainmenu_play, 'application/xml');
      Array.from(doc.querySelectorAll('snippet')).forEach(function (n) { menus[n.getAttribute('name')] = n.firstElementChild; });
    },
    build: shell,
    attach: function () { publish(); if (current !== 'workshop' && $.Filmmaker) $.Filmmaker.gotoTab('downloaded'); },
    tick: function () {
      if (!ctx || !ctx.IsValid()) return;
      ['FilmmakerMenuTab', 'MvmPlayMenuHost'].forEach(function (id) {
        var host = ctx.FindChildTraverse(id); if (!host) return;
        var queue = host.GetAttributeString('cmd', ''); if (!queue) return;
        host.SetAttributeString('cmd', ''); queue.trim().split('\n').forEach(handle);
      });
    },
    reset: function () { model = JSON.parse(JSON.stringify(fixture)); commands.length = 0; global.MvmNativeMenu.selectedMap = null; publish(); },
    commands: function () { return commands.slice(); },
    model: function () { return model; }
  };
})(window);
