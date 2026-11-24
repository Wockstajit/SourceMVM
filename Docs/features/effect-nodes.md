# Effect Nodes

`CONFIG > NODES` builds a post-process graph: mask nodes pick a part of the frame, effect nodes
change it, and the Output node mixes the result over the scene. The graph editor
(`NodesGraphOverlay`) draws the nodes and wires in game and can be dragged.

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
([render passes](filmmaker_render_passes.md)).

## Console

```text
mirv_filmmaker nodes on|off|toggle|reset
mirv_filmmaker nodes preset <name>
mirv_filmmaker nodes canvas open|close|toggle
mirv_filmmaker nodes add <type> | select <id> | remove <id> | enable <id> <0|1> | move <id> <x> <y>
mirv_filmmaker nodes connect <fromId> <toId> [toPort 0|1] | disconnect <edgeId>
```

## Code and tests

- [EffectNodes/](../../AfxHookSource2/Filmmaker/EffectNodes) (model and compiler, the D3D11
  evaluator, the graph editor script)
- `EffectNodesMathTests`: presets, cycles, and mask slot lifetimes (a shared mask outliving later
  masks, a deep Mask Combine tree, a graph with too many live masks)
