# Effect Nodes

`CONFIG > NODES` builds a post-process graph: mask nodes pick a part of the frame, effect nodes
change it, and the Output node mixes the result over the scene. The tab holds the starter presets,
a search over every node type to add, the graph's nodes in a list that scrolls inside its own box,
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
| Drag empty space, or turn the wheel | Pan |
| Right-click a node | On or off, Duplicate, Disconnect, Delete |
| Right-click a wire | Disconnect |
| Right-click empty space, or **+ Add node** | The add menu: type to search, Enter adds the first match |
| Delete / Ctrl+D | Delete / duplicate the selected node (not Final Output) |
| Title bar / corner | Move / resize the window; it remembers both (`nodes.graph`) |

**Frame all** fits every node in the window, and the window frames the graph the first time it opens,
so Final Output is never left off screen. A wire that makes no sense is refused with the reason in
the title bar: a mask cannot be an image input (Final Output's or an effect's port 0), an image
cannot be a mask input, and no wire may close a loop (`CheckConnect` in EffectNodesMath.h).

| Kind | Nodes |
| --- | --- |
| Masks | Depth Range, Sky Mask, Player Matte, Fog Mask, Luma Key, Mask Combine (union, intersect or subtract of two masks, or invert) |
| Effects | Color Grade, Glow, Outline, Distortion. Port 0 is the image, port 1 the mask |
| Output | Final mix (`p[0]`) |

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
