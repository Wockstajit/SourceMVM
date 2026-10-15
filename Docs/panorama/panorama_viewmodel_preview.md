# CS2 Panorama Weapon / Viewmodel 3D Preview

This file covers how CS2 renders a live 3D weapon and arms (viewmodel) model inside a Panorama
panel. That is the kind of preview shown in the Settings ▸ Visuals ▸ Viewmodel tab, with the knife
and hands on the left and the controls on the right. It companions `panorama_ui_guide.md`, which
covers the surrounding UI. The findings come from CS2's native Panorama panel types and from how
Osiris drives them (`Source/CS2/Panorama/CUI_3dPanel.h`, `CUI_Item3dPanel.h`, and
`Source/UI/Panorama/Tabs/VisualsTab/ViewmodelModPreviewPanel.h`).

## The short version

CS2 renders the weapon for you. Its native Panorama panel types own a small
offscreen 3D scene (a "portrait world") and render a model into the panel's texture. To use one:

1. Create a native 3D item panel from JS (`MapItemPreviewPanel`).
2. Point it at a weapon or item by **item definition id** (which weapon), plus an **attributes**
   string (skin, wear, stattrak, etc.).
3. Let its built-in camera (`camera_weapon_0`) frame the weapon. Optionally, trigger a "look at"
   animation.

The arms and hands come from the viewmodel scene the preview world loads. They are part of the
`camera_weapon` portrait setup. You do not add them separately.

## The native panel types

```text
CPanel2D                      // base Panorama panel
  └── CUI_3dPanel             // a panel that owns a 3D portrait scene + camera
        └── CUI_Item3dPanel   // a 3D panel specialized for econ items (weapons/knives/gloves)
```

### CUI_3dPanel (`Source/CS2/Panorama/CUI_3dPanel.h`)

The generic type that renders a 3D scene into a panel. Relevant members:

```cpp
struct CUI_3dPanel : CPanel2D {
    using PortraitWorld = CCS_PortraitWorld*;  // the offscreen mini-scene that holds the model + lights
    using Fov           = float;               // camera field of view
    using FovWeight     = float;               // blend weight when interpolating FOV
};
```

`CCS_PortraitWorld` is the self-contained scene. It owns the model entity, lighting, and the camera
definitions (e.g. `camera_weapon_0`). The panel renders that world to a composition layer and draws
it as the panel's content. The main menu uses the same machinery for agent and loadout previews.

### CUI_Item3dPanel (`Source/CS2/Panorama/CUI_Item3dPanel.h`)

Specializes `CUI_3dPanel` for economy items. Relevant members:

```cpp
struct CUI_Item3dPanel : CUI_3dPanel {
    // Load a specific item (weapon/knife/glove) into the scene.
    //   itemId     : the item id to display (econ item / definition reference)
    //   attributes : extra attributes string (paint kit, wear, seed, stattrak, name tag, ...)
    using SetItemItemId    = void (*)(CUI_Item3dPanel* thisptr, ItemId itemId, const char* attributes);

    // Animate the camera to "look at" the loaded weapon (the subtle inspect framing).
    using StartWeaponLookAt = bool (*)(CUI_Item3dPanel* thisptr);
};
```

- **`SetItemItemId`** chooses *which* weapon and *how it looks*. The first argument selects the
  weapon, knife, or glove. The `attributes` string carries the cosmetic state (paint kit id,
  float/wear, pattern seed, StatTrak, name tag). This is what shows a specific skinned knife
  instead of the default model.
- **`StartWeaponLookAt`** starts the camera framing and inspect animation on the weapon.

## Driving it from Panorama JS (recommended)

For a filmmaker tool, you rarely need to call the C++ methods directly. The native panel can be
created and configured from JS with attributes, which survives CS2 updates better:

```js
var preview = $.CreatePanel('MapItemPreviewPanel', parent, 'WeaponPreview', {
  map: 'ui/xpshop_item',          // the preview "world" / scene to load
  camera: 'camera_weapon_0',      // the weapon framing camera defined in that world
  'require-composition-layer': true,
  player: false,                  // false = item/weapon preview (not a full player)
  initial_entity: 'item',
  mouse_rotate: false,            // set true to let the user spin the model
  sync_spawn_addons: true,
  'transparent-background': true, // so it composites over your panel background
  'pin-fov': 'vertical',
  style: 'width: 700px; height: 400px;'
});

preview.SetHideStaticGeometry(true); // hide the map/world geometry, keep just the model + arms
```

Notes:

- `MapItemPreviewPanel` is the XML/JS tag name. `CUI_Item3dPanel` is the C++ type behind it.
- `map: 'ui/xpshop_item'` with `camera: 'camera_weapon_0'` gives the weapon-with-arms framing. The
  store and viewmodel previews use this pairing. The arms and hands are part of that portrait world.
- `SetHideStaticGeometry(true)` removes the surrounding scene geometry. The result is a weapon and
  hands on a transparent background.
- For a full player model, use the sibling type `MapPlayerPreviewPanel` (backed by the player
  portrait world). It works the same way with a different scene and camera.

## Choosing the weapon / skin shown

There are two ways, depending on how much control you need:

1. **Attribute-driven (simplest):** set the item attributes on the panel in JS, so CS2 resolves the
   model itself. This works well for showing the currently equipped knife or loadout weapon.
2. **Explicit (`SetItemItemId`):** resolve the panel's C++ `CUI_Item3dPanel*` through your Panorama
   bridge or handle, then call `SetItemItemId(itemId, attributes)` to force a specific weapon and paint
   kit. Call `StartWeaponLookAt()` to frame it. Use this when the preview should follow filmmaker
   config, for example "preview the AK with this skin and wear for offline filming".

## Why this is useful for a filmmaker tool

- **Viewmodel FOV preview:** show the live arms and weapon while the user drags an FOV slider.
- **Weapon, knife, and glove preview:** pick the exact item and skin to film with, offline.
- **Loadout and agent preview:** use `MapPlayerPreviewPanel` for player models and agents.

All of it is native CS2 rendering inside a Panorama panel. It needs no separate overlay or manual
DirectX, and it composites with the rest of the UI.

## Risks / notes

- The C++ method signatures (`SetItemItemId`, `StartWeaponLookAt`) and the panel layout are the
  fragile, reverse-engineered part. Keep them isolated in the Panorama bridge layer (see
  the "How to resolve these at runtime" section of `panorama_ui_guide.md`). The JS attribute names are far more
  stable across CS2 updates.
- The portrait-world camera names (`camera_weapon_0`) and the preview map (`ui/xpshop_item`) are
  Valve assets that an update could rename. Keep them in the JS layer so a fix never touches C++.
- This is for offline, demo, and menu use, consistent with the rest of the HLAE filmmaker design.

## Source references

```text
CS2 native 3D panel types (current Osiris master):
  Source/CS2/Panorama/CUI_3dPanel.h
  Source/CS2/Panorama/CUI_Item3dPanel.h

Osiris viewmodel preview that drives MapItemPreviewPanel from JS:
  Source/UI/Panorama/Tabs/VisualsTab/ViewmodelModPreviewPanel.h

Companion UI-creation guide:
  Docs/panorama/panorama_ui_guide.md
```
