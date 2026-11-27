# Effect Nodes

`CONFIG > NODES` builds a post-process graph: mask nodes pick a part of the frame, effect nodes
change it, and the Output node mixes the result over the scene. The tab holds the starter presets,
a search over every node type to add (grouped into masks and effects), the graph's nodes in a list that scrolls inside its own box,
and the selected node's inspector.

## The graph editor

**Open Graph Editor** shows the graph in its own window, drawn above the TIMELINE dock and the demo
bar (it lives in Config's overlay layer). It shows only while CONFIG is open on the NODES tab, so
`nodes canvas open` alone shows nothing until then. It works the way Blender and Nuke do:

| Do | To |
| --- | --- |
| Press a node and drag | Select it and move it |
| Drag from an output pin to an input | Connect them; the input's old wire is replaced |
| Drop a wire on empty space | Pick a node from the add menu; it arrives wired to that output |
| Drag from a connected input | Pick its wire up again |
| Drag empty space | Pan |
| Wheel / Shift+wheel / Ctrl+wheel | Zoom about the pointer (25% to 200%) / pan sideways / pan up and down |
| Right-click a node | On or off, Duplicate, Disconnect, Delete |
| Right-click a wire | Disconnect |
| Right-click empty space, or **+ Add node** | The add menu: type to search, Enter adds the first match |
| Delete / Ctrl+D | Delete / duplicate the selected node (not Final Output) |
| Title bar / corner | Move / resize the window; it remembers both (`nodes.graph`) |

**Frame all** zooms out until every node fits in the window (never past 100%), and the window frames the graph the first time it opens,
so Final Output is never left off screen. A wire that makes no sense is refused with the reason in
the title bar: a mask cannot be an image input (Final Output's or an effect's port 0), an image
cannot be a mask input, and no wire may close a loop (`CheckConnect` in EffectNodesMath.h).

| Kind | Nodes |
| --- | --- |
| Masks from the scene | Depth Range, Sky Mask, Player Matte, Fog Mask, Height Mask (a band of world height), Edge Mask (where depth jumps), Around Player (a sphere around the picked player, or what the camera looks at) |
| Masks from the picture | Luma Key, Hue Key (one colour), Radial Mask, Box Mask, Gradient Mask, Noise Mask (drifting, animated) |
| Masks from masks | Mask Combine (union, intersect or subtract of two masks, or invert), Mask Feather (blur a mask, then grow or shrink it) |
| Colour | Color Grade, Lift Gamma Gain, Tonemap (filmic, soft or punchy), Split Tone, Hue Shift, Duotone, Color Fill (normal, multiply, screen, overlay or add) |
| Light and lens | Glow, Glare (streaks, four or six point stars, ghosts, fog glow), Halation, Light Rays, Vignette, Depth Fog, Lens Distortion, Chromatic Aberration, Film Grain |
| Blur and bend | Blur, Depth Blur (focus distance and range), Directional Blur, Radial Blur (zoom or spin), Tilt Shift, Sharpen, Distortion, Refraction (moving noise, or bending at a mask's edges) |
| Stylise | Cel Shade (banded shading, ink on depth and crease edges, hatching: the comic look of Borderlands), Ink Lines, Kuwahara (painted), Outline, Posterize, Pixelate, Halftone, Mirror, Invert |
| Devices and damage | Night Vision, Thermal, Film Damage, Analog Damage (VHS), Scanlines, Glitch |
| Output | Final mix (`p[0]`) |

Every effect takes the image on port 0 and an optional mask on port 1, and blends its result over
the image by mask times Mix. The type numbers are what the shaders switch on (`NodeType` in
EffectNodesMath.h); new types are appended and existing numbers never move.

Night Vision and Thermal are modelled on the real devices. Night Vision lifts the shadows the way an
image intensifier does, sparkles with scintillation that is strongest in the dark, blooms bright
sources into halos, and shows one or two tubes with a faint honeycomb and a falloff at the rim.
Thermal maps a heat estimate (surfaces by the light they hold, the sky cold, people hot with a halo)
through White Hot, Black Hot, Ironbow or Sepia, with a soft, noisy sensor and an optional scope.
Thermal with people heat turned up needs the world reference render, like a Player Matte. The tube
or scope sway, the scintillation and Analog Damage's tape jitter run on the wall clock (`stepInfo.w`),
so they keep moving while the demo is paused.

Presets: color the subject and desaturate the world, glow through the fog, player neon outline,
depth haze and grade, sky tint and subject pop. Each one is an ordinary graph to edit afterwards.

## How a graph runs

`CompileGraph` in [EffectNodesMath.h](../../AfxHookSource2/Filmmaker/EffectNodes/EffectNodesMath.h)
walks back from Output and emits every mask before the effects that read it. Cycles are ignored.
Masks live in four GPU slots (`kMaskSlots`). `AssignMaskSlots` gives a mask a slot nothing still
needs, and frees it only after the last node that reads it has run, so a mask that feeds several
effects, or the first branch of a large Mask Combine tree, is never overwritten before use. A graph
that would need a fifth mask alive at the same moment drops that mask and the nodes that read it
(`droppedForMaskSlots`) instead of drawing with the wrong mask.

Player Matte nodes isolate the player picked for the Selected player pass
([render passes](filmmaker_render_passes.md)), or every player while `selectedOnly` is off or nobody
is picked. The two player presets follow the pick.

## The live player mask

A Player Matte compares the scene depth with a World Only depth, so a graph with one adds a second
render of each frame (`m_LiveWorldRefPending` in `RenderSystemDX11Hooks.cpp`). That render draws over
the same back buffer after Main, so Main is shown first: `EngineThread_BeginMainRenderPass` queues the
present on Main's own commands, together with `AfxSceneToBackBuffer`, which copies the finished scene
target across. CS2 resolves its MSAA scene target only for its own Present, so without the copy the
present shows the reference render of the frame before. The reference render is never shown. The mask
lags the scene by one frame, so `MaskPS` looks each pixel up through the view the reference was
rendered with (`AfxRenderPass::WorldReferenceView`). A recording adds the same reference render.

## Console

```text
mirv_filmmaker nodes on|off|toggle|reset
mirv_filmmaker nodes preset <name>
mirv_filmmaker nodes canvas open|close|toggle
mirv_filmmaker nodes add <type> [x y] [link <fromId>] | select <id> | remove <id> | duplicate <id>
mirv_filmmaker nodes unlink <id> | enable <id> <0|1> | move <id> <x> <y> | param <id> <idx 0..7> <val>
mirv_filmmaker nodes connect <fromId> <toId> [toPort 0|1] | disconnect <edgeId>
```

## Code and tests

- [EffectNodes/](../../AfxHookSource2/Filmmaker/EffectNodes) (model and compiler, the D3D11
  evaluator, the graph editor script)
- `EffectNodesMathTests`: presets (the player presets follow the pick), cycles, and mask slot lifetimes (a shared mask outliving later
  masks, a deep Mask Combine tree, a graph with too many live masks)
  and the connect rules (`CheckConnect`, `DefaultLinkPort`)
- `effect-nodes-ui-tests.mjs`: the graph editor's gestures, keys, overlay layer and the in-place
  restyling that keeps buttons from blinking
- `$.ConfigHud.nodesPointer(x, y, lmb, rmb)` feeds the graph a pointer over netcon, so its gestures
  can be driven without the mouse; call it with no arguments to hand the pointer back
