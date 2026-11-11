#pragma once

// ROUTES fragment of the onboarding panel (concatenated into kOnboardingJs after
// OnboardingScriptJs.h, shares its scope): the paths through the steps, the deeper-rundown menu and
// its topics, and the endings. Route names match Onboarding.cpp's kRoutes.
//
//   cs2 / app          the two opening paths. Each gives the gist, then offers the deeper rundown.
//   more               the deeper-rundown menu.
//   keys, graph, path, render, recoil, fov, config   one topic each; their end leads back to `more`.
//
// Plain ASCII on purpose. Non-ASCII punctuation is written as a JS \u escape.

R"OBJS(
    // ---- weapon recoil and bob (the cs2 path and the `recoil` topic)
    var RECOIL_STEPS = [
      openConfigStep(T('Open CONFIG.'), T('This is where the camera and the gun get their feel. Open CONFIG down here.')),
      { ctx: 'hud', title: T('Weapon Recoil.'), expr: 'curious', target: 'cfg:config.section.weaponrecoil:header', keys: ['SPACE'], needs: { config: true, pov: true, card: 'config.section.weaponrecoil' },
        enter: function () { cfgCard('config.section.weaponrecoil'); seek(T_AK); },
        body: T('This kicks the gun itself when he fires: it punches back, the muzzle climbs and it rolls a little. Switch it on.'),
        done: function (a) { return a.recoil; }, act: function () { raw('mirv_filmmaker weaponrecoil on'); } },
      { ctx: 'hud', title: T('Press {0}.', K('recoil.test', 'T')), expr: 'happy', action: T('Next'), target: 'cfg:config.section.weaponrecoil:test', keys: ['T', 'SPACE'], needs: { config: true, pov: true, card: 'config.section.weaponrecoil' },
        act: function () { raw('mirv_filmmaker weaponrecoil test'); },
        body: T('With CONFIG open, {0} fires one test kick, even on a paused demo. Watch where the gun swings from. {1} plays the AK spray.', K('recoil.test', 'T'), K('demo.pause', 'SPACE')) },
      { ctx: 'hud', title: T('The pivot is the hinge.'), expr: 'focused', action: T('Next'), target: 'cfg:config.section.weaponrecoil:pivot', keys: ['T'], needs: { config: true, pov: true, card: 'config.section.weaponrecoil' },
        body: T('Click this drawer open. Every gun kicks around a hinge: push Forward out toward the muzzle and the barrel swings while the grip stays planted. Pull it back toward you and the whole gun sweeps across the screen.') },
      { ctx: 'hud', title: T('AK or Glock, different hinge.'), expr: 'curious', action: T('Next'), target: 'cfg:config.section.weaponrecoil:pivot', keys: ['SPACE', 'T'], needs: { config: true, pov: true, card: 'config.section.weaponrecoil' },
        enter: function () { seek(T_GLOCK); },
        body: T("Press {0}: he swaps the AK for a Glock. Each weapon class has its own hinge, a rifle's out on the barrel and a pistol's near the hand. Change Forward and press {1} to feel it.", K('demo.pause', 'SPACE'), K('recoil.test', 'T')) }
    ];
    var BOB_STEPS = [
      { ctx: 'hud', title: T('Weapon bob.'), expr: 'focused', action: T('Next'), target: 'cfg:config.section.weaponbob:header', keys: ['SPACE'], needs: { config: true, pov: true, card: 'config.section.weaponbob' },
        enter: function () { cfgCard('config.section.weaponbob'); seek(T_KNIFE_WALK); },
        act: function () { raw('mirv_filmmaker weaponbob on'); },
        body: T('Bob is the sway while he walks, and it comes from his real movement: speed, footsteps, landing, crouching and how fast he turns. Switch it on and press {0} for this knife walk.', K('demo.pause', 'SPACE')) },
      { ctx: 'hud', title: T('Lighter guns bob less.'), expr: 'happy', action: T('Got it'), target: 'cfg:config.section.weaponbob:pivot', keys: ['SPACE'], needs: { config: true, pov: true, card: 'config.section.weaponbob' },
        body: T('A knife barely moves, a rifle swings the most. Bob has its own hinge too, so the sway can come from the wrist or the shoulder.') }
    ];
    var FOV_STEPS = [
      openConfigStep(T('Open CONFIG.'), T('The lens settings are in CONFIG.')),
      { ctx: 'hud', title: T('Field of view.'), expr: 'focused', action: T('Next'), target: 'cfg:config.section.viewmodel:header', needs: { config: true, card: 'config.section.viewmodel' },
        enter: function () { cfgCard('config.section.viewmodel'); },
        body: T("Field of view sets how wide the whole shot is. 90 is CS2's normal view. Lower is tight and telephoto, higher feels fast and stretched.") },
      { ctx: 'hud', title: T('Weapon FOV is its own thing.'), expr: 'curious', action: T('Next'), target: 'cfg:config.section.viewmodel:header', needs: { config: true, card: 'config.section.viewmodel' },
        body: T('It changes how big the gun looks without touching the world. It works in Shared mode.') },
      openEditorStep(T('Now CAM EDITOR.'), T('Each camera key has its own zoom too.')),
      KEYS_READY, DOCK_CAMERA, SELECT_KEY, PATH_TAB, KEY_FOV
    ];
    var CONFIG_STEPS = [
      openEditorStep(T('Open CAM EDITOR.'), T("Start in the editor, like you're halfway through a shot.")),
      { ctx: 'hud', title: T('Click here for CONFIG.'), expr: 'curious', target: 'ModeSwapBtn', needs: { editor: true, dock: 'native' },
        body: T('This button swaps to CONFIG without losing your place. Your camera keys stay where they are.'),
        done: function (a) { return a.config; }, act: function () { raw('mirv_filmmaker config on'); } },
      { ctx: 'hud', title: T('Now swap back.'), expr: 'happy', target: 'ModeSwapBtn', needs: { config: true, dock: 'native' },
        body: T("This is where you'll tweak effects, lighting, ragdolls and how the camera feels. Not yet, though: on the tour you can only click what I light up. Swap back with the same button."),
        done: function (a) { return a.editor; }, act: function () { raw('mirv_filmmaker editor on'); } }
    ];

    function deeper() {
      return { ctx: 'any', title: T('Want the deeper rundown?'), expr: 'curious',
        body: T("That's the gist. I can walk you through rendering, scene keys, the graph, camera keys, recoil and the lens, one at a time. It won't take forever. Probably."),
        options: [{ label: T('Show me more'), go: 'more 0' }, { label: T("I'm good"), go: 'finale' }] };
    }
)OBJS"
R"OBJS(
    var ROUTES = {
      cs2: { name: T('CS2 filmmaking'), steps: [DEMOS_TAB, DOWNLOADED_TAB, REPAIR, WATCH].concat(RECOIL_STEPS, BOB_STEPS, [deeper()]) },
      app: { name: T('SOURCE:MVM tour'), steps: [
        DEMOS_TAB, DOWNLOADED_TAB, ADD, SORT, REPAIR, WATCH,
        openEditorStep(T('Two workspaces.'), T('CAM EDITOR builds camera moves. CONFIG holds effects, lighting, ragdolls and how the camera feels. Open CAM EDITOR.')),
        CONFIG_STEPS[1], CONFIG_STEPS[2],
        PLACE_CAMS, DOCK_CAMERA, SELECT_KEY, KEY_EASE, PLAY_PATH
      ].concat(RENDER_STEPS.slice(2), [deeper()]) },

      // ---- the deeper rundown: a menu, then one topic at a time
      more: { name: T('Deeper rundown'), steps: [
        { ctx: 'any', title: T('Pick a topic.'), expr: 'curious',
          body: T('Each one is a walk-through in the practice demo, and I take you to the right moment. Back here when it ends.'),
          options: [{ label: T('Rendering your shot'), go: 'render 0' }, { label: T('Scene keys: the look over time'), go: 'keys 0' },
                    { label: T('The graph'), go: 'graph 0' }, { label: T('Camera keys up close'), go: 'path 0' },
                    { label: T('Recoil and weapon bob'), go: 'recoil 0' }, { label: T('Field of view'), go: 'fov 0' },
                    { label: T('CONFIG while you edit'), go: 'config 0' }] }
      ] },
      render: { name: T('Rendering'), topic: true, steps: RENDER_STEPS },
      keys: { name: T('Scene keys'), topic: true, steps: SCENE_STEPS },
      graph: { name: T('The graph'), topic: true, steps: GRAPH_STEPS },
      path: { name: T('Camera keys'), topic: true, steps: [
        EDITOR_OPEN, KEYS_READY, DOCK_CAMERA, SELECT_KEY, PATH_TAB, KEY_FOV, KEY_EASE, KEY_SMOOTH, KEY_REPOS, PLAY_PATH] },
      recoil: { name: T('Recoil and weapon bob'), topic: true, steps: RECOIL_STEPS.concat(BOB_STEPS) },
      fov: { name: T('Field of view'), topic: true, steps: FOV_STEPS },
      config: { name: T('CONFIG while you edit'), topic: true, steps: CONFIG_STEPS }
    };

    var FINALE = { title: T("Sorry, I'm falling for you."), expr: 'love', action: T('Pick another path'),
      body: T("That shot was too good. Pick another path or go explore. Type mvm_onboarding in the console whenever you want me back.") };
    // The close box asks before ending. Stopping keeps the place: mvm_onboarding comes back to the
    // same step and sets it up again.
    var EXIT_ASK = { title: T('Leave the tutorial?'), expr: 'curious',
      body: T("You can stop here. Type mvm_onboarding in the console any time and I'll pick up right where you left off."),
      options: [{ label: T('Keep going'), go: '@resume' }, { label: T('Stop here'), go: '@off' }, { label: T('Start over'), go: '@restart' }] };
    var TOPIC_END = { title: T("That's that one."), expr: 'happy', action: T('Back to topics'),
      body: T('Pick another topic, or go make something.') };
    // Shown in place of a step that belongs to the other context.
    function bridgeLine(step) {
      if (step.ctx === 'hud') return { title: T("Let's open the practice demo."), expr: 'happy', action: T('Load practice demo'),
        body: T('This part happens inside a demo. I will load the practice one: a short round on Italy with a couple of fights to film.') };
      return { title: T("That part's on the main menu."), expr: 'curious', action: T('Skip ahead'),
        body: T("You're already in a demo, so we'll skip the menu bits. They are there whenever you go back to the main menu.") };
    }
    // Poke him three times quickly and he swoons. Shown for a moment, then the lesson line returns.
    var SWOON = { title: T('Easy, director.'), expr: 'love',
      body: T("Poke me like that again and I'll fall for you. Oh no. Too late.") };
)OBJS"
