// NO `#pragma once`, on purpose: one .cpp can hold several scripts (Community has four, the
// Play menu three), and each of them splices this fragment in. With an include guard only the
// first script in the file got it and the others stopped at "T is not defined" (seen in game).

// LOCALIZATION RUNTIME -- one fragment, spliced into every Panorama script body.
//
// NOT a standalone script. It is a raw-string chunk #included inside a host IIFE, right after the
// IIFE opens, and gives that script three locals:
//
//   T('Save & Apply')                    the text in the active language, or the English given
//   T('Closest {0}', ratioName)          {0} {1} ... are filled AFTER the lookup, so a translation
//                                        may put them in another order
//   TN(count, '{0} key', '{0} keys')     picks the plural form for `count` in the active language;
//                                        {0} is the count, further arguments are {1} {2} ...
//   K('marker.place', 'K')               the cap text of the key a shortcut is on now (the user
//                                        can rebind it, Input/Keybinds.h), or the default given
//   mvmLabel(parent, id, props)          $.CreatePanel('Label', ...) whose text shrinks to fit
//
//   T('Clear##weather')                  the same, for an English word used in two senses: the
//                                        part after ## names the sense and is never shown
//
// The English text is the key (Docs/panorama/localization.md). A string with no entry in the
// active table is shown in English, so a missing translation is never an empty label.
//
// WHY A FRAGMENT IN EVERY SCRIPT rather than one script run first: the scripts are injected by
// about thirty independent drivers into two Panorama contexts (the in-game HUD and the menu), and
// any of them can be the first to run after a context reload. Each one therefore installs the
// shared object if it is missing and then aliases it. The table itself is pushed by
// Localization.cpp ($.MvmI18n.set), which also bumps `rev` so panels know to rebuild.
//
// Wrap the LITERAL, at the place it is written: lbl(p, T('Reset')). Never pass T() a value that
// is also an id, a console argument, a persist key or anything compared with ===; translate at the
// point of display instead. Automation/i18n/extract-strings.mjs reads these calls to build the
// catalog, so the first argument must be a plain string literal.

R"MVMI18N(
    if (!$.MvmI18n || $.MvmI18n.version !== 1) {
      $.MvmI18n = (function () {
        var api = { version: 1, lang: 'english', rev: 0, table: {}, keys: {}, missing: null };
        // CLDR cardinal rules for whole numbers, reduced to the index of the form to use. The
        // order of forms in a table entry is the order listed here per language.
        //   one, other                      most European languages
        //   one (0 and 1), other            French, Brazilian Portuguese
        //   one, few, many                  Russian, Ukrainian, Polish
        //   one, few, other                 Czech, Romanian
        //   other                           Chinese, Japanese, Korean, Thai, Vietnamese, Indonesian
        var rules = {
          one_other: function (n) { return n === 1 ? 0 : 1; },
          zero_one_other: function (n) { return (n === 0 || n === 1) ? 0 : 1; },
          east_slavic: function (n) {
            var d = n % 10, h = n % 100;
            if (d === 1 && h !== 11) return 0;
            if (d >= 2 && d <= 4 && (h < 12 || h > 14)) return 1;
            return 2;
          },
          polish: function (n) {
            var d = n % 10, h = n % 100;
            if (n === 1) return 0;
            if (d >= 2 && d <= 4 && (h < 12 || h > 14)) return 1;
            return 2;
          },
          czech: function (n) { return n === 1 ? 0 : (n >= 2 && n <= 4 ? 1 : 2); },
          romanian: function (n) {
            var h = n % 100;
            if (n === 1) return 0;
            if (n === 0 || (h >= 1 && h <= 19)) return 1;
            return 2;
          },
          none: function () { return 0; }
        };
        var ruleOf = {
          french: 'zero_one_other', brazilian: 'zero_one_other',
          russian: 'east_slavic', ukrainian: 'east_slavic', polish: 'polish',
          czech: 'czech', romanian: 'romanian',
          schinese: 'none', tchinese: 'none', japanese: 'none', koreana: 'none',
          thai: 'none', vietnamese: 'none', indonesian: 'none'
        };
        function fill(text, args, first) {
          if (args.length <= first || text.indexOf('{') < 0) return text;
          return text.replace(/\{(\d)\}/g, function (whole, digit) {
            var value = args[first + (+digit)];
            return value === undefined ? whole : ('' + value);
          });
        }
        function note(key) { if (api.missing && api.lang !== 'english') api.missing[key] = 1; }
        api.t = function (text) {
          var hit = api.table[text];
          if (typeof hit !== 'string') {
            // 'Clear##weather': the part after ## only tells two uses of one English word apart
            // for the translator. It is never shown.
            var mark = text.indexOf('##');
            hit = mark < 0 ? text : text.substr(0, mark);
            note(text);
          }
          return fill(hit, arguments, 1);
        };
        api.tn = function (count, one, other) {
          var n = Math.abs(Math.floor(+count || 0));
          var key = one + ' || ' + other, forms = api.table[key], text;
          if (forms && forms.length) {
            var index = rules[ruleOf[api.lang] || 'one_other'](n);
            text = forms[index < forms.length ? index : forms.length - 1];
          } else { text = n === 1 ? one : other; note(key); }
          var args = [count];
          for (var i = 3; i < arguments.length; i++) args.push(arguments[i]);
          return fill(text, args, 0);
        };
        // Replaces the table. `rev` changes only when the language or its text really changed, so
        // a driver that re-pushes the same table after a context repair does not rebuild panels.
        api.key = function (action, fallback) { return api.keys[action] || fallback || ''; };
        api.set = function (lang, table, stamp, keys) {
          var same = api.lang === lang && api.stamp === stamp;
          api.lang = lang; api.table = table || {}; api.stamp = stamp; api.keys = keys || {};
          if (!same) api.rev++;
          return api.rev;
        };
        return api;
      })();
    }
    var T = $.MvmI18n.t, TN = $.MvmI18n.tn, K = $.MvmI18n.key;
    // Every text label is created through here (directly, or by the script's own mk / lbl
    // helpers), so every label shrinks its text to fit rather than cutting it off. Translated
    // text is longer than the English the layouts were drawn for; measured in game, `shrink`
    // scales a caption down inside a fixed-width parent even when the label itself is
    // auto-width, and on a wrapping label it only acts on a word too long for the line. A
    // label that sets its own textOverflow afterwards (an ellipsis by design) keeps it.
    function mvmFit(panel) { try { panel.style.textOverflow = 'shrink'; } catch (e) {} return panel; }
    function mvmLabel(parent, id, props) { return mvmFit($.CreatePanel('Label', parent, id || '', props || {})); }
)MVMI18N"
