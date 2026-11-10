#pragma once

// RAGDOLLS tab of the CONFIG panel. JS fragment concatenated (via #include, as an adjacent raw
// string literal) into the kConfigHudJs script body assembled in ConfigHudJs.h -- not a
// standalone script. Shares scope with the root IIFE, and appends to `pageRagdolls` /
// `renderHooks`.
//
// Its own tab (rather than a corner of EFFECTS) because ragdoll work is a separate feature
// area -- see RagdollPhysics.h (physics) and
// RagdollWeapon.h.
//
// Three independent cards on purpose: the death-body retune, death-weapon styling and physics knobs
// are different features with different failure modes, and pretending they are one toggle would make
// it impossible to tell which of them is responsible for how a body behaves.

R"CFJS(
    // ===================== ADVANCED RAGDOLLS ============================
    // Death Animations (Movie/DeathAnimations.h): the dying player plays a baked death clip
    // exactly, the way the Blender viewer shows it, and the ragdoll only takes over once the clip
    // is over. One clip family at a time. Names are prefixed `da`: the fragments share one scope.
    // Two cards (2026-10 sidebar cleanup): DEATH ANIMATIONS and NERVOUS SYSTEM, each with its own
    // master switch in the header and on the rail, like every other card. The nervous system
    // still needs death animations on; its rail summary says so while they are off.
    var rgSec = section(pageRagdolls, T('DEATH ANIMATIONS'), true, true, 'ragdoll.model');
    var rgReset = rgSec.__headerReset(['mirv_filmmaker deathanim reset'], 'rag.deathanim');
    var daTog = rgSec.__headerPill(function () {
      cmd('mirv_filmmaker deathanim ' + (st && st.daOn ? 'off' : 'on'));
    }, T('Enables death animations and, with its own switch, the Nervous System. '
      + 'Tune how closely the body follows the clip below.'));
    var daPage = rgSec;
    var nsSec = section(pageRagdolls, T('NERVOUS SYSTEM'), true, false, 'ragdoll.nervous');
    var nsReset = nsSec.__headerReset(['mirv_filmmaker nervous reset'], 'rag.nervous');
    var nsTog = nsSec.__headerPill(function () {
      cmd('mirv_filmmaker nervous ' + (st && st.nsOn ? 'off' : 'on'));
    }, T('Shot players keep reacting on the ground for a few seconds before they go still: a hand goes to '
      + 'the wound, the chest heaves, the body writhes and twitches. Headshots go limp at once. '
      + 'Needs Death animations on; reactions start after the body lands and the animation lets go.'));
    var nsPage = nsSec;
    var DA_KEYS = ['mw2019', 'classic', 'extra'];
    var daSource = modeRow(daPage, T('Animations'),
      [['mw2019', 'MW2019'], ['classic', T('Classic CoD')], ['extra', T('Extra')]], '260px',
      function (key) { cmd('mirv_filmmaker deathanim source ' + key); },
      T('MW2019: Modern Warfare 2019. Classic CoD: World at War, Black Ops 1 and 2, MW3. '
        + 'Extra: the GMod death pack. Applies to the next death.'));
    // How the clip plays on the body. Plain words on purpose: these are the knobs people reach for.
    var daBlendIn = sliderRow(daPage, {
      label: T('Blend in'), cmd: 'mirv_filmmaker deathanim blendin',
      min: 0, max: 1, decimals: 2, unit: ' s',
      tip: T('How long the body takes to go from the pose the player died in into the animation. '
        + 'Short is a sharp reaction; long is a softer, slower start.')
    });
    var daStrength = sliderRow(daPage, {
      label: T('Animation strength'), cmd: 'mirv_filmmaker deathanim strength',
      min: 10, max: 100, decimals: 0, unit: '%',
      tip: T('How strong the body is while it plays the animation. 100% follows it closely. Lower is '
        + 'weaker: at 30% the body can barely hold itself up and sags under gravity, at 10% it is a '
        + 'ragdoll the animation only nudges.')
    });
    var daImpact = sliderRow(daPage, {
      label: T('Grenade blast'), cmd: 'mirv_filmmaker deathanim impact',
      min: 0, max: 300, decimals: 0, unit: '%',
      tip: T('How hard a grenade throws the body. It flies out and up and the animation plays while it '
        + 'flies. Gun deaths are never pushed. 0 plays grenade deaths in place. Needs Solid body.')
    });
    var daBlendOut = sliderRow(daPage, {
      label: T('Blend out'), cmd: 'mirv_filmmaker deathanim blendout',
      min: 0, max: 1, decimals: 2, unit: ' s',
      tip: T('How long the animation crossfades into the ragdoll before it lets go: before the body '
        + 'lands, at the end of the animation, and after it hits a wall. The ragdoll picks up the '
        + 'motion smoothly instead of the body snapping loose. 0 lets go at once.')
    });
    var daWalls = togglePill(daPage, T('React to walls'),
      T('Arms and legs that hit something bend or go limp; a body that hits a wall stops the animation.'),
      function () { cmd('mirv_filmmaker deathanim walls ' + (st && st.daWalls ? 'off' : 'on')); },
      T('A limb pushed a little by a wall or object bends against it; pushed hard, it goes limp while '
        + 'the rest of the animation keeps playing. If the chest, hips or head hit something, the '
        + 'animation stops and physics takes over from there.'));
    var daSolid = togglePill(daPage, T('Solid body'),
      T('The body collides with floors, walls and itself while the animation plays.'),
      function () { cmd('mirv_filmmaker deathanim solid ' + (st && st.daSolid ? 'off' : 'on')); },
      T('On: the animation steers the body, and physics keeps knees, feet and arms out of the floor '
        + 'and walls. Off: the body follows the animation exactly, even if that means sinking '
        + 'into the ground or passing through a wall.'));
    var daPickBtn = wideBtn(daPage, T('PER-PLAYER ANIMATIONS'), function () {
      if ($.ConfigHudDap) $.ConfigHudDap.open();
    }, S.accent);
    daPickBtn.style.marginTop = '10px';
    attachTip(daPickBtn, T('Pick a player and the clip their deaths always play, with a looping preview '
      + 'of each clip.'));
    var daInfo = lbl(daPage, '', S.hint, 12);
    daInfo.style.whiteSpace = 'normal'; daInfo.style.width = '100%';
    daInfo.style.paddingLeft = UI.line.padH + 'px'; daInfo.style.paddingRight = UI.line.padH + 'px';
    daInfo.style.marginTop = '8px';

    renderHooks.push(function (st) {
      daTog.set(!!st.daOn);
      var src = (typeof st.daSource === 'number') ? st.daSource : 0;
      daSource.sync(DA_KEYS[src] || 'mw2019');
      daBlendIn.sync(st.daBlendIn);
      daStrength.sync(st.daStrength);
      daImpact.sync(st.daImpact);
      daBlendOut.sync(st.daBlendOut);
      daWalls.set(!!st.daWalls);
      daSolid.set(!!st.daSolid);
      var counts = st.daClips || [0, 0, 0];
      var n = counts[src] || 0;
      daInfo.text = !st.daOn ? T('Off. Deaths use the normal ragdoll.')
        : (!st.daLoaded ? T('Loading clips...')
        : (n > 0 ? TN(n, '{0} clip. Hit location and direction pick the clip.',
                     '{0} clips. Hit location and direction pick the clip.')
                   + (st.daAssigned > 0 ? ' ' + T('{0} player(s) have their own.', st.daAssigned) : '')
                   + (st.daActive > 0 ? ' ' + T('Playing on {0}.', st.daActive) : '')
                 : T('No clips for this source: {0}', st.daStatus || '?')));
    });

)CFJS"
R"CFJS(
    // ---- NERVOUS SYSTEM ---------------------------------------------------
    // Movie/NervousSystem.h: a shot body keeps reacting on the ground for a few seconds -- a hand to
    // the wound, a shot leg drawn up, breathing, writhing and twitches -- then fades out.
    // Headshots go limp at once. The Death Animations master gates it (nsTog is its header switch).
    // Prefix `ns`: the fragments share one scope.
    subHead(nsPage, T('Reactions'));
    var nsReach = togglePill(nsPage, T('Reach for the wound'),
      T('Hands go to where the shot landed. A leg hit grabs the leg and draws it up, a stomach hit '
        + 'curls up with both hands on it, an arm hit clutches it with the other hand.'),
      function () { cmd('mirv_filmmaker nervous reach ' + (st && st.nsReach ? 'off' : 'on')); });
    var nsBreath = togglePill(nsPage, T('Breathing'),
      T('The chest heaves, fast at first and slowing down, with one last breath at the end.'),
      function () { cmd('mirv_filmmaker nervous breath ' + (st && st.nsBreath ? 'off' : 'on')); });
    var nsWrithe = togglePill(nsPage, T('Writhing'),
      T('Arms, legs and head move in pain. Grenade and fire deaths thrash and kick their legs.'),
      function () { cmd('mirv_filmmaker nervous writhe ' + (st && st.nsWrithe ? 'off' : 'on')); });
    var nsTwitch = togglePill(nsPage, T('Twitches'),
      T('Short spasms of a hand, foot or the head, more of them as the body fades.'),
      function () { cmd('mirv_filmmaker nervous twitch ' + (st && st.nsTwitch ? 'off' : 'on')); });
    var nsRollOver = togglePill(nsPage, T('Roll over'),
      T('A body that lands face down tries to turn onto its back before reaching for the wound.'),
      function () { cmd('mirv_filmmaker nervous rollover ' + (st && st.nsRollOver ? 'off' : 'on')); });
    subHead(nsPage, T('Timing'));
    var nsStrength = sliderRow(nsPage, {
      label: T('Reaction strength'), cmd: 'mirv_filmmaker nervous strength',
      min: 10, max: 200, decimals: 0, unit: '%',
      tip: T('How hard the body can move while it reacts. Low is a weak, fading struggle; high is a '
        + 'violent one. Physics always keeps the body on the floor and out of walls.')
    });
    var nsDuration = sliderRow(nsPage, {
      label: T('Reaction length'), cmd: 'mirv_filmmaker nervous duration',
      min: 2, max: 20, decimals: 1, unit: ' s',
      tip: T('Roughly how long a body reacts before it goes still. Each death varies it, and the wound '
        + 'changes it: a neck or blast death fades faster, a leg or arm wound lasts longer.')
    });
    var nsBlendIn = sliderRow(nsPage, {
      label: T('Reaction blend in'), cmd: 'mirv_filmmaker nervous blendin',
      min: 0, max: 2, decimals: 2, unit: ' s',
      tip: T('How gradually the reaction starts once the body is down (or once the death animation lets '
        + 'go). Longer is a softer, more natural hand-off.')
    });
    var nsBlendOut = sliderRow(nsPage, {
      label: T('Reaction blend out'), cmd: 'mirv_filmmaker nervous blendout',
      min: 0, max: 4, decimals: 2, unit: ' s',
      tip: T('How gradually the movement dies away before the body goes still. Hands slide off the '
        + 'wound and the breathing settles instead of stopping at once.')
    });
    var nsInfo = lbl(nsPage, '', S.hint, 12);
    nsInfo.style.whiteSpace = 'normal'; nsInfo.style.width = '100%';
    nsInfo.style.paddingLeft = UI.line.padH + 'px'; nsInfo.style.paddingRight = UI.line.padH + 'px';
    nsInfo.style.marginTop = '8px';

    renderHooks.push(function (st) {
      // The switch shows the backend's own state. With Death animations off it is ARMED, not
      // active, and the note and rail summary say so; hiding it as "off" let a click arm it
      // silently, and switching Death animations on later started reactions nobody asked for.
      nsTog.set(!!st.nsOn);
      nsStrength.sync(st.nsStrength);
      nsDuration.sync(st.nsDuration);
      nsBlendIn.sync(st.nsBlendIn);
      nsBlendOut.sync(st.nsBlendOut);
      nsReach.set(!!st.nsReach);
      nsBreath.set(!!st.nsBreath);
      nsWrithe.set(!!st.nsWrithe);
      nsTwitch.set(!!st.nsTwitch);
      nsRollOver.set(!!st.nsRollOver);
      nsInfo.text = !st.daOn ? (st.nsOn
          ? T('Armed: reactions start as soon as Death animations are switched on.')
          : T('Death animations are off, so the nervous system is inactive. Switch them on first.'))
        : !st.nsOn ? T('Nervous system off. Bodies go limp when they die.')
        : T('Where the shot landed decides the reaction. Headshots go limp at once.')
          + (st.nsActive > 0 ? ' ' + T('Reacting: {0}.', st.nsActive) : '');
      // Each card's own summary. The death animations card names its clip source and count; the
      // nervous system card lists the reactions that are on, or says what it is waiting for.
      var srcName = ['MW2019', T('Classic CoD'), T('Extra')][(typeof st.daSource === 'number') ? st.daSource : 0];
      var clipCounts = st.daClips || [0, 0, 0], clipN = clipCounts[(typeof st.daSource === 'number') ? st.daSource : 0] || 0;
      rgSec.__note(st.daOn ? (srcName + (clipN > 0 ? ' · ' + TN(clipN, '{0} clip', '{0} clips') : '')) : T('Off'));
      var reactions = [];
      if (st.nsReach) reactions.push(T('reach')); if (st.nsBreath) reactions.push(T('breathing'));
      if (st.nsWrithe) reactions.push(T('writhing')); if (st.nsTwitch) reactions.push(T('twitches'));
      if (st.nsRollOver) reactions.push(T('roll over'));
      nsSec.__note(!st.daOn ? (st.nsOn ? T('Armed · needs Death animations') : T('Off · needs Death animations')) : (!st.nsOn ? T('Off')
        : (reactions.length ? reactions.join(' · ') : T('On · no reactions picked'))));
    });

)CFJS"
R"CFJS(

    // ===================== DEATH WEAPON ================================
    var rwSec = section(pageRagdolls, T('DEATH WEAPON'), true, false, 'ragdoll.weapon');
    var rwReset = rwSec.__headerReset(['mirv_filmmaker ragdollweapon reset'], 'rag.deathweapon');
    var rwKeepTog = rwSec.__headerPill(function () { cmd('mirv_filmmaker ragdollweapon keep '
        + (st && st.ragdollKeepWeapon ? 'off' : 'on')); },
      T('Keeps the weapon in the right hand for the hold duration, then releases it into native weapon physics.'));
    var rwFireTog = togglePill(rwSec, T('Involuntary firing'),
      T('Each selected death produces a visual trigger squeeze with recoil, muzzle flash, '
        + 'ejected casing and tracer. Enabling also enables weapon hold. No real bullets.'),
      function () { cmd('mirv_filmmaker ragdollweapon fire '
        + (st && st.ragdollDeathFire ? 'off' : 'on')); });

    var rwHold = sliderRow(rwSec, {
      label: T('Hold duration'), cmd: 'mirv_filmmaker ragdollweapon holdms',
      min: 1, max: 10000, decimals: 0, unit: ' ms',
      tip: T('Time in the hand before releasing with the hand’s momentum, then tumbling and settling through weapon physics. Applies to new deaths; pauses follow demo time.')
    });
    var rwChance = sliderRow(rwSec, {
      label: T('Firing chance'), cmd: 'mirv_filmmaker ragdollweapon chance',
      min: 0, max: 100, decimals: 0, unit: '%',
      tip: T('100% fires on every armed death. 0% never fires. Seeking repeats the same random selection.')
    });
    var rwDuration = sliderRow(rwSec, {
      label: T('Firing duration'), cmd: 'mirv_filmmaker ragdollweapon firems',
      min: 1, max: 5000, decimals: 0, unit: ' ms',
      tip: T('Starts with an immediate shot; further shots use the held gun\'s firing interval. Snipers fire once. Stops on release. '
        + 'Very short durations produce one shot on the first available frame.')
    });

    var rwGroundNote = lbl(rwSec, '', S.hint, 12);
    rwGroundNote.style.whiteSpace = 'normal'; rwGroundNote.style.width = '100%';
    rwGroundNote.style.paddingLeft = UI.line.padH + 'px';

    // ===================== PHYSICS ======================================
    // Every knob here is a CS2 convar (see RagdollPhysics.cpp) -- no reversing, and nothing
    // that breaks on a game update beyond a convar rename.
    var rpSec = section(pageRagdolls, T('PHYSICS'), true, false, 'ragdoll.physics');
    var rpReset = rpSec.__headerReset(['mirv_filmmaker ragdollphys reset'], 'rag.physics');
    var rpNote = lbl(rpSec, T('Affects bodies as they fall. Existing bodies keep the physics they landed with.'),
      S.hint, 12);
    rpNote.style.whiteSpace = 'normal'; rpNote.style.width = '100%';
    rpNote.style.paddingLeft = UI.line.padH + 'px'; rpNote.style.paddingRight = UI.line.padH + 'px';

    var rpGravity = sliderRow(rpSec, {
      label: T('Gravity'), cmd: 'mirv_filmmaker ragdollphys gravity',
      min: -2.0, max: 4.0, decimals: 2, unit: 'x',
      tip: T('1.00 is normal. Below 1 floats bodies for slow-motion shots; above 1 drops them '
         + 'hard. Negative lifts them upward.')
    });
    var rpFriction = sliderRow(rpSec, {
      label: T('Friction'), cmd: 'mirv_filmmaker ragdollphys friction',
      min: 0.0, max: 4.0, decimals: 2, unit: 'x',
      tip: T('CS2 default is 0.60. Lower slides bodies along the ground; higher makes them stick '
         + 'where they land.')
    });
    var rpLimit = sliderRow(rpSec, {
      label: T('Max bodies'), cmd: 'mirv_filmmaker ragdollphys limit',
      min: 1, max: 64, decimals: 0, unit: '',
      tip: T('How many corpses stay in the world at once. CS2 default is 20; raise it so bodies '
         + 'from earlier in a round are still there for a wide shot.')
    });
    var rpMinAge = sliderRow(rpSec, {
      label: T('Keep for'), cmd: 'mirv_filmmaker ragdollphys minage',
      min: 0, max: 300, decimals: 0, unit: 's',
      tip: T('Seconds before the engine may recycle a body. CS2 default is 10.')
    });

    // Bodies collide is an on/off, so it is a checkbox chip (Docs/features/filmmaker_config_panel.md) -- same command.
    var rpCollideBtn = togglePill(rpSec, T('Bodies collide'),
      T('Lets ragdolls collide with each other so they pile up instead of sinking through one another. Off is the CS2 default.'),
      function () { cmd('mirv_filmmaker ragdollphys collide ' + ((st && st.rp_collide >= 0.5) ? '0' : '1')); });

    // Reset lives in the card header; the one action stays in the body, right-aligned.
    var rpActions = actionGroup(rpSec, '', [[T('Clear bodies'), function () {
      cmd('mirv_filmmaker ragdollphys cleanup');
    }]]);
    var rpClearBtn = rpActions.btns[0];

    // ===================== METHOD ACTING ================================
    var maSec = section(pageRagdolls, T('METHOD ACTING'), true, false, 'acting');
    var maReset = maSec.__headerReset(['mirv_filmmaker methodacting reset'], 'rag.acting');
    var maTog = maSec.__headerPill(function () {
      cmd('mirv_filmmaker methodacting ' + (st && st.actingOn ? 'off' : 'on'));
    }, T('Subtle eye movement and reactions. Works on agents with animated eyes.'));
    var maBlinkTog = togglePill(maSec, T('Blink naturally'),
      T('Irregular blinks with occasional double blinks. Timing is automatic.'),
      function () { cmd('mirv_filmmaker methodacting blink ' + (st && st.actingBlink ? 'off' : 'on')); });
    var maSquintTog = togglePill(maSec, T('Combat squint'),
      T('Squint while firing, when hit, near incoming shots, and while blinded by a flashbang.'),
      function () { cmd('mirv_filmmaker methodacting squint ' + (st && st.actingSquint > 0 ? '0' : '0.5')); });
    var maGazeTog = togglePill(maSec, T('Look around'),
      T('Held left and right glances, occasional head turns, and brief looks toward nearby players.'),
      function () { cmd('mirv_filmmaker methodacting gaze ' + (st && st.actingGaze ? 'off' : 'on')); });
    var maDeathTog = togglePill(maSec, T('Relax eyes on death'),
      T('Eyes roll upward and slightly aside, then rest under partly open lids.'),
      function () { cmd('mirv_filmmaker methodacting deatheyes ' + (st && st.actingDeathEyes ? 'off' : 'on')); });
    renderHooks.push(function (st) {
      maTog.set(!!st.actingOn);
      maBlinkTog.set(!!st.actingBlink);
      maSquintTog.set(st.actingSquint > 0);
      maGazeTog.set(!!st.actingGaze);
      maDeathTog.set(!!st.actingDeathEyes);
      maSec.__note(st.actingOn ? T('Natural eye reactions') : T('Off'));
    });

    renderHooks.push(function (st) {
      rwKeepTog.set(!!st.ragdollKeepWeapon);
      rwFireTog.set(!!st.ragdollDeathFire);
      rwHold.sync(st.ragdollHoldDurationMs);
      rwChance.sync(st.ragdollFireChance);
      rwDuration.sync(st.ragdollFireDurationMs);
      rwGroundNote.text = st.ragdollPhysicsWaiting > 0
        ? T('Weapon physics unavailable for {0} weapon(s).', st.ragdollPhysicsWaiting) : '';
      rpGravity.sync(st.rp_gravity);
      rpFriction.sync(st.rp_friction);
      rpLimit.sync(st.rp_limit);
      rpMinAge.sync(st.rp_minage);
      var colOn = (st.rp_collide >= 0.5);
      rpCollideBtn.set(colOn);

      rwSec.__note(st.ragdollKeepWeapon
        ? (st.ragdollDeathFire ? T('Held · {0}% firing', st.ragdollFireChance) : T('Held'))
        : (st.ragdollDeathFire ? T('Firing armed · enable hold') : T('Off')));
      rpSec.__note(T('Gravity {0}', typeof st.rp_gravity === 'number' ? st.rp_gravity.toFixed(2) : '1.00')
        + (typeof st.rp_limit === 'number' ? ' · ' + TN(Math.round(st.rp_limit), '{0} body', '{0} bodies') : '')
        + (colOn ? ' · ' + T('collide') : ''));
      rdKeep.sync(st);
      rdReset.paint();
    });
)CFJS"
R"CFJS(
    // ===================== RAGDOLLS FOOTER ==============================
    // The same footer CAMERA and EFFECTS end with. `ragdollphys reset` is the physics module's own
    // (it owns those convar defaults); the model and death-weapon switches default off.
    var rdFooter = footerCard(pageRagdolls);
    var rdKeep = keepToggle(rdFooter, 'ragdolls', 'keepRagdolls',
      T('the death animation, nervous system, death-weapon, physics and Method Acting settings'));
    var rdReset = resetFooter(rdFooter, function () {
      return { label: T('RAGDOLLS'), cmds: ['mirv_filmmaker effects reset ragdolls'] };
    }, 'ragdolls');
)CFJS"
