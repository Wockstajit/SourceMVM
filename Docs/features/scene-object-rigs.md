# Scene object rigs: attachments and pose keys

Placed lights, meshes and cameras can ride things in a CS2 demo and can be keyed over time. This
file is the feature contract. Keep it current with the code.

Related: [environment-scene-editor.md](environment-scene-editor.md) (the Scene Objects outliner and
lights), [camera-editor-feature.md](camera-editor-feature.md) (the TIMELINE dock and scene keys),
[follow-camera](../follow-camera/README.md) (the target and mount resolution this reuses).

## What it does

A placed scene object (a point, spot or area light, an imported mesh, or a **camera** object) can be:

1. **Attached** to something in the demo: a player at a body mount, a player's **held weapon**, a
   **dropped weapon** or the C4, a thrown **grenade**, or **another scene object**. It rides that
   parent and keeps a local **offset** (position and angles).
2. **Placed from the free cam**: fly to where it should be and press **Place at camera**. A light or a
   camera takes the view's direction, a camera also its field of view. A mesh keeps its own turn.
3. **Keyed over time** with **Animate** on. Every placement or pose edit keys the whole pose at the
   snapped playhead. Pose keys are **linear**: placed at tick A and again at tick B, the object moves
   from pose A to pose B. Moving or turning it at a key makes both of that key's segments linear
   again, even after a **Hold**. The keys can be moved, deleted, held, eased or curved on the
   TIMELINE dock like any scene key.

Attachments and keys persist per demo in the `scenekeys` section of the demo's `project.mvm`. The
objects themselves, and their authored world pose, stay in the map's Environment scene.

### Not in this version

- Keying the attachment itself (attach at tick A, detach at B). An attachment is one setting per
  object. To release an object mid-shot, duplicate it: one attached, one keyed in the world.
- CS2 asset props (Scene Objects > Assets, `Filmmaker/Props`). They are engine entities placed in
  the late frame through another path. Only lights, imported meshes and cameras can be rigged.
- Smoothing, prediction or dead-zone filters on the ride. An attached object is a rigid mount. Use
  the Follow Camera for a smoothed ride.
- Deadlock.

## Using it

Select a light, mesh or camera in **CONFIG > ENVIRONMENT > Scene Objects > Outliner**. Its card has a
**RIG** section under the light or mesh inspector. **Camera** in the card's **+ Add** menu (beside
the point, spot and area lights) adds a camera object 160 units ahead of the view, facing the same
way, with the view's field of view.

| Control | What it does |
| --- | --- |
| **Attach to** | Nothing, a player, a player's weapon, a dropped weapon, a thrown grenade, or another scene object. Picking a kind fills **Which**; picking an entry there attaches. |
| **Which** | The players, the projectiles and the dropped weapons at this tick, or the other placed objects. A parent that is gone at this tick still shows by name. |
| **Mount** | The point it rides: head, eyes, chest, pelvis, feet, right or left hand, or the entity origin on a player; muzzle, shell eject, magazine, bolt, slide or the entity on a weapon. The list comes from the live model when it can. |
| **While it is gone** | **Hide** (default) or **Hold last pose**. A parent is gone before a grenade is thrown and after it ends, while a player has no pawn or skeleton, when a dropped weapon is picked up, and when a parent object is removed or switched off. |
| **Offset** | Forward / Right / Up and Pitch / Yaw / Roll in the mount's frame (x forward, y right, z up, the Follow Camera's convention). A camera also has **Field of view**. |
| **Place at camera** | Moves it to the camera. When it rides something, the offset is solved so it lands exactly there. |
| **Animate** | On: pose edits and placements key it at the playhead. Off: the keys are removed (it asks first when there are several) and the pose at the playhead becomes its rest pose. |
| **Go to camera** | Cameras only: goes to the camera's tick, then puts the free cam where it looks from. Its tick is its first pose key, or the throw when it rides a grenade. When the playhead is already inside its keys, or it has neither, nothing is seeked. |
| **Look through** | Cameras only: goes to the camera's tick the same way, then the game renders from this camera until you stop. |
| **Open timeline** | Opens the TIMELINE dock, where the object is a layer. |

Attaching keeps the object where it is on screen. Its current world pose is re-expressed in the new
mount's frame. With Animate on, that rewrites the key under the playhead and leaves the other keys'
numbers as they are, so attach first and then animate. Detaching also keeps it in place: the world
pose becomes its authored pose, or a key at the playhead when it is animated. Changing the mount
re-solves the offset the same way.

The gizmo's arrows and the light or mesh card's turn sliders move a rigged object through its rig.
The arrows drag it in the world and the drag is converted into its offset, or keyed when it is
animated. The Pitch / Yaw / Roll rows at the top of a light or mesh card stay there when it is
rigged and show its angles in the frame it rides. Every light shows them, a point light too: they
are keyed with its pose and show in the timeline even though a point light does not use them. The
RIG card adds Forward / Right / Up for an attached object, and the full turn and field of view for a
camera, which has no other card. A slider drag is one undo step. A world position is set with the
arrows or **Place at camera**: a slider over the whole map would move it a hundred units a pixel.

Chains are allowed: a light on a camera that rides a player. A chain is at most eight deep and can
never loop back on itself. A chain whose root is gone is gone.

### On the TIMELINE dock

Every scene camera and light has an **OBJECTS · Camera 3** layer, keyed or not, and any other
object has one once it has keys. A camera's rows are **Position**, **Rotation** and **Field of
view**. A light's are Position, Rotation, **Brightness**, **Range**, **Beam angle** (spot and area),
**Emitter size** (point and spot) and **Width** / **Height** (area). A listed object is not animated
until its layer switch is turned on (that is Animate on) or a key is placed on one of its rows; a
key on a light setting animates that setting alone. **Delete object** in the layer's right-click menu
removes the object (undoable); right-click > **Add** (also on empty layer space) adds a camera or a
point, spot or area light where you look. Right-click a row to **Separate Dimensions** like the
camera's own rows. A new pose key is linear. A held key's glyph shows the half square of a hold on its
outgoing half, and the next key's incoming half matches.

- **Keyframe Assistant > Hold** (also the **Hold** button on the ease strip) makes the segments
  leaving the selected keys steps. This works on any scene key, not only object poses. Moving the
  object at a key (an arrow, a turn row, **Place at camera**) clears the holds on both of that key's
  segments, so the move blends again (user request 2026-10-17: a held segment jumped).
- **Go to a key**: double-click it, on any row and in either view, or right-click it and pick
  **Go to this key**.
- **Linear**, **Smooth** and the **Easy Ease** variants turn a held segment back into a curve on the
  side they name. **Ease In** frees the incoming segment, **Ease Out** the outgoing one.
- Dragging a Bezier handle in GRAPH frees the segment that handle belongs to.
- GRAPH draws a hold as a step. Position and field-of-view strips fit the values they hold rather
  than the +-32768 range.
- Before the first key an object sits at the first key, and after the last key it stays at the last.
  It does not return to a baseline the way CONFIG properties do outside their keys.
- The layer switch disables the keys (the object goes back to its rest pose); deleting the layer
  removes them. A removed object's layer leaves the dock at once. Its keys stay in the project and
  come back with the layer if the removal is undone. Object ids are never reused, so a new object
  never inherits them.

### Looking through a camera object

**Look through** makes that camera the view. It is applied after the camera path, the Follow Camera
and the free cam, so it outranks all three. Only the Rust view hook runs after it. Hold right-click to
look around the free cam underneath, and press **Stop looking through** to return to it. One camera
can be looked through at a time, and it resets when the demo changes. The live shake stack
(Handheld, Weapon Recoil) treats an attached camera like the Follow Camera's attach mode and a placed
or keyed one like a path.

A camera object has its own authored **camera shake**: the **Camera shake** row of its OBJECTS
layer, which every camera object gets on the TIMELINE dock, also before it has pose keys. It is the
camera path's shake model, store and menus (`CamPathShake`, `pathshake obj <id> ...`): keys on any
tick, each running until the next key or a Stop, Intensity and Speed drawn in GRAPH. It shakes the
view while that camera is looked through, at the scene keys' tick, and the path's shake never
reaches it ([filmmaker_camera_path_shake.md](filmmaker_camera_path_shake.md)).

### In the world

The scene's cameras and lights are drawn in the world as solid, shaded markers, one colour per kind
(`SceneMarkerMath.h`), with crisp edges on a dark underline so they read on a sunlit wall as well as
in shade:

| Marker | Colour | Shape |
| --- | --- | --- |
| Camera object | teal | a camera body and a glass lens at its field of view, with a triangle for up |
| Point light | orange | a bulb, glowing in the light's own colour |
| Spot light | pink | a lamp with its lens lit in the light's colour and a fading beam at its cone |
| Area light | lime | a panel of its size, lit on its front in the light's colour, with a short fading shaft |
| Camera path key (K) | gold (Live) / blue (Freeze) | the same camera as a camera object |
| Follow Camera | purple | the same camera |
| Follow target | cyan | a glass box with crisp edges |

The selected object, and the camera path key you aim at, are lit and edged white. A keyed object
that rides nothing also draws the path its keys take in its kind's colour, with a dot on every key,
sampled through its curves, eases and holds (a hold shows as a step). The camera path's line takes
its Live or Freeze colour. An attached object draws only where it is now: its keys are an offset
from a parent whose other poses are not known. Config's Scene Objects icons ring each object in the
same colour and print its kind in it (`accent` in the props state). The overlay shows while Config
is closed and while the Follow Camera owns the view. It hides with F8 and while a camera path
plays, never reaches a capture, and skips the camera being looked through. `rig overlay off` turns
it off.

## Console

```text
mirv_filmmaker environment add camera
mirv_filmmaker rig status [<id>]
mirv_filmmaker rig parent <id> none
mirv_filmmaker rig parent <id> player <pawnIndex> [<mount>]
mirv_filmmaker rig parent <id> held <pawnIndex> [<mount>]
mirv_filmmaker rig parent <id> dropped <entityIndex>
mirv_filmmaker rig parent <id> grenade <entityIndex>|h<handle>
mirv_filmmaker rig parent <id> object <otherId>
mirv_filmmaker rig mount <id> <mount>
mirv_filmmaker rig missing <id> hide|hold
mirv_filmmaker rig animate <id> on|off
mirv_filmmaker rig place <id>
mirv_filmmaker rig set <id> x|y|z|pitch|yaw|roll|fov <value>
mirv_filmmaker rig offset <id> <x> <y> <z> [<pitch> <yaw> <roll>]
mirv_filmmaker rig lookthrough <id> on|off
mirv_filmmaker rig goto <id>
mirv_filmmaker rig overlay on|off
mirv_filmmaker scenekeys graph hold
```

`rig status` lists every rigged or animated object with its parent, mount, resolved entity, the
mount resolver's source tag, whether the parent is valid, its local and world pose and its key count.
The entity indices for `parent` are the ones `rig status`, `follow status` and the RIG card's pickers
show at the current tick. `rig set` on an object that is neither attached nor animated edits its
authored world pose.

`mvm_debug` categories: `rig.resolve` (once a second while logging, at the main frame and again at
view setup: parent kind, entity, mount, source, valid and oriented, local and world pose, visible),
`rig.attach`, `rig.place`, `rig.animate` and `rig.view` (look-through camera pose, throttled).

## How it works

```text
Filmmaker/Movie/SceneRigMath.h        engine-free: Attachment, compose/decompose, chain resolution,
                                      missing policy, channel ids and numbers, attach JSON
Filmmaker/Movie/SceneRig.{h,cpp}      main-thread pump, parent lookup and Follow mount resolver,
                                      view-setup resample, `rig` commands, Config state JSON
Filmmaker/Movie/SceneRigTransient.h   immutable per-frame poses read by the Environment's capture
Filmmaker/Movie/SceneKeyframeTrackMath.h  linear pose keys, clamped ends, object channel specs,
                                      attach map, SamplePositionPath (the overlay's paths)
SceneOverlayData.h                    the overlay's cameras, lights and paths, handed whole to
                                      CampathDrawer (drawn next to the camera path)
SceneMarkerMath.h                     engine-free: one colour per kind, the markers' shading, a spot
                                      light's beam, their size on screen (drawer, rig and Props)
CampathDrawer.cpp                     the solid markers: triangles through the line shader with no
                                      extrusion, near faces only, far to near
Filmmaker/Movie/SceneKeys.cpp         object layers in the dock state, Hold, the rig's edit API
Filmmaker/Environment/EnvironmentScene.*  camera objects, never-reused ids, rig overlay, rig-aware
                                      gizmo and pose edits
Panorama/ConfigHudRigJs.h             the RIG card
Panorama/GraphEditorJs.h              OBJECTS layers, grouped rows, hold glyphs and steps, Hold
```

### Data

Each object animates through seven pose channels: `obj.<id>.x`, `.y`, `.z`, `.pitch`, `.yaw`,
`.roll` and `.fov`, and a light through six more: `.intensity` (0 to 8), `.range` (16 to 4096),
`.cone` (5 to 170), `.radius` (0 to 64), `.width` and `.height` (1 to 1024), the scene file's own
ranges. Keyed light settings reach the renderer with the pose (`SceneRigTransient.h`). Their specs are built on demand (`ObjectSpec`), so a project whose objects are
gone still loads. Positions are floats in +-32768, angles wrap in (-180, 180], and the field of view
is 1 to 179. These specs set `editLinear` (writing a value at a key clears the holds on both of
its segments) and `clampEnds` (the ends extend outward). The dock numbers object channels `1000000 + id * 16 + channel`, above the static catalog, so
adding or removing an object never renumbers a channel under a drag.

Each key carries `hold` (saved as `"hold": true`, absent means false). It makes the segment leaving
that key a step. The track's `attach` map holds one record per attached object: the parent kind, its
identity, the mount, the missing policy and the **offset**. The offset lives in the per-demo record,
not in the map's scene, so the same light can ride a player in one demo and sit in the world in
another. Attachments share the scene keys' undo history.

```json
"attach": {
  "3": { "parent": "player", "steamId": "76561198000000123", "name": "ropz", "mount": "head",
         "missing": "hide", "offset": [20, 0, 4, 0, 0, 0] },
  "5": { "parent": "grenade", "type": "Smoke", "handle": "8913021", "throwTick": 18340, "thrower": "ropz",
         "mount": "entity", "missing": "hold", "offset": [0, 0, 16, 0, 0, 0] },
  "6": { "parent": "dropped", "entity": 212, "class": "weapon_ak47", "mount": "entity",
         "missing": "hide", "offset": [0, 0, 0, 0, 0, 0] },
  "7": { "parent": "object", "object": 3, "mount": "", "missing": "hide", "offset": [8, 0, 0, 0, 0, 0] }
}
```

A parent is found again every frame by what survives the demo:

- **Player** and **held weapon**: the controller's Steam id, then its name. Held follows the pawn's
  active weapon through switches.
- **Grenade**: its entity handle first (the demo records it, so a seek gives the same grenade the
  same handle; `handle` is optional, "0" or absent when unknown), then its type and throw tick
  (within 4 ticks), the thrower breaking ties. The projectile found is kept while its handle stays
  at that index; otherwise a scan runs every eighth frame until the throw happens.

**Which grenade** lists every grenade thrown within a minute of the playhead, in the air or seen
earlier this session. The list, the Camera Editor's Follow → Grenade list and `follow type
grenade` share one index (`GrenadeIndex_Update` in `FollowTargetProviders.cpp`). A grenade's
throw tick comes from the demo, so it holds across seeks: in the air before its first bounce it is
exact (its distance from the launch point at the launch speed); for a smoke that has popped it is
the pop tick less the least time it took to get there; otherwise the earliest tick it was ever seen
with. CS2's own trail time is client-side and restarts on every seek, so it is only the last
resort.
- **Dropped weapon**: the entity index and designer class, valid while that index holds that class
  with no owner. This is the weakest identity.
- **Scene object**: its id.

The Environment scene file gains `nextObjectId` (optional, never below one past the highest id) and a
camera's `fov`. Cameras count against their own limit of 16.

### Each frame (main thread)

1. `SceneKeys_RunFrame` applies the CONFIG and Environment keys and records the tick it applied, the
   capture tick while rendering.
2. `SceneRig_RunMainThreadFrame` takes the loaded map's objects. Each one's local pose is its keys
   sampled at that tick over its rest pose (the offset when attached, the authored pose otherwise).
   Each entity parent is looked up and read through `ResolveAttachTransform` in rigid mode. Chains
   are composed, the missing policy is applied, and the rigged objects' world poses are published to
   `SceneRigTransient`.
3. At view setup (`main.cpp`, after the Follow Camera) the parents found in step 2 are read again.
   That pose is the interpolated one the models are drawn at, so a light on a head moves smoothly
   instead of stepping at the tick rate. The poses are published again, and a looked-through camera
   writes the view.
4. `Environment::CaptureFrame` on the engine thread overlays the published poses onto its copy of
   the scene. A hidden object is switched off for that frame. Picking, the hover dot and the gizmo use
   the same overlay.

Entity memory is read only on the main thread and at view setup, where the Follow Camera already
reads it. The capture reads an immutable snapshot.

## Verification

Native tests: `scene-rig-math-tests.cpp` (compose and decompose at pitched and rolled parents,
chains, loops, the depth limit, the missing policy, channel ids and numbers, the attach format) and
`scene-keyframe-track-tests.cpp` (linear keys, holds cleared by a re-pose, clamped ends, re-keying,
hold and attach round trips, undo of an attach, the overlay's sampled paths and key dots) and
`scene-marker-math-tests.cpp` (the palette's kinds stay apart in hue, shading, the view frame, near
faces, a spot light's beam, the screen size). JS: `scene-rig-ui-tests.mjs` checks that the RIG card, the dock, the mock and
the native command surface agree. Mockup views: `env_rig`, `env_rig_light`, `grapheditor_rig` and
`grapheditor_rig_graph` in `UiMockup/capture-mock.mjs`.

Live check (de_dust2, `replays\yashfartt.dem`), with `mvm_debug start` first:

1. Add a spot light and attach it to a player's head. Run `rig status` at several ticks with a window
   capture at each one. The light's world position tracks the head, and `rig.resolve` logs valid=1.
2. Attach to the held weapon's muzzle and play through a weapon switch.
3. Attach to a grenade. Before the throw it is hidden, after the throw it rides.
4. Turn Animate on, place at tick A, play on, place at tick B. Capture at A, between A and B, at B and
   after B to show the blend. Hold the first key and capture the jump; move the object at B and
   check the hold is gone.
5. Look through an attached camera object across a scrub. `rig.view` logs the pose.
6. Quit with `quit`, relaunch the demo, and check that the attachments and keys are back.
