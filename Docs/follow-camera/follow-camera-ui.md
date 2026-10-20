# Follow / Attached camera UI

The Follow inspector is in the camera editor (`mirv_filmmaker editor on`). Its implementation is
[CameraEditorFollowInspectorJs.h](../../AfxHookSource2/Filmmaker/Panorama/CameraEditorFollowInspectorJs.h).

## Modes and settings

The mode selector stays visible on the Target, Framing and Tracking pages.

| Mode | Camera behavior | Framing | Tracking |
|---|---|---|---|
| Lock-on | Stays at the placed position and turns toward the target | World X/Y/Z aim offsets, aim rotation trim, FOV | Aim smoothing, target smoothing, prediction, deadzone, maximum turn rate, death and lost-target behavior |
| Attached | Moves and rotates with the selected mount | Local X forward, Y right, Z up; rotation trim and FOV | Rigid mount with render-time sampling; death and lost-target behavior |

Each mode remembers its own framing, FOV, mount and tracking settings for the session. Switching modes
restores those settings. Reset framing and Reset tracking affect only the current mode. Target
selection and the placed Lock-on camera stay shared.

Attached does not display the aim sliders. Its backend uses zero smoothing and prediction to keep the
camera fixed to the mount.

## Death and lost targets

Both modes provide an **On player death** section for Player targets and held-weapon targets:

- **Keep following** follows the moving ragdoll while its skeleton remains available.
- **Timed hold** follows it for **Death hold** playback seconds, then stops. Zero stops immediately.
- For Player targets, **Retarget dropped weapon / C4** waits for Death hold, then switches to the
  dropped item. If both are enabled, the C4-first preference applies.
- The duration appears when Timed hold or a retarget option uses it. Otherwise the UI explicitly says
  no death timer is active.
- **Hold last camera pose** applies when the target or corpse disappears. It does not freeze a ragdoll
  that is still available.

The timer counts demo ticks once each. Pause does not consume it, and slow motion scales it with
playback. Seeking resets the active window. Starting Live on an already dead target starts a new hold
from that preview.

Attached Head follows the skull bone. Chest and Pelvis follow spine bones. Their saved position and
orientation stay relative to those bones while alive and dead. Eyes follows the player's view while
alive, then a calibrated head mount after death. Feet remains a virtual offset from the lower spine after
death rather than a separate ankle bone. If the model lacks a required body attachment, lost-target
behavior applies.

Weapon switches preserve the requested mount and its framing. Muzzle aliases resolve to the model's
native muzzle point. If a drawn weapon has no such point (for example a knife), the camera temporarily
rides the grip. Switching back restores the requested gun point. A saved point that is absent from the
current model stays visible in the Mount dropdown.

## Layout

| Area | Controls |
|---|---|
| Persistent header | Live/Stop, Place/Replace and Clear (Lock-on), Reposition, mode selector, status |
| Target | Type, candidate selection, Go To, player/source for weapons, mount for Attached, recorded events |
| Framing & Tracking | One card with two sub-tabs and one Reset for both. FRAMING: seven value rows under Offset, Rotation and Lens, with the active mode's axes explained. TRACKING: the Aim rows, the On death policy and the lost-target policy |

Reposition places the Lock-on camera, or captures the freecam pose relative to the current Attached
mount. Weapon and grenade selection use the event and candidate lists.

## Automation

The controls use the [Follow console commands](follow-camera-commands.md). `follow state64` returns the
complete JSON state in encoded chunks, including active-mode settings, `targetDead`, `targetValid`,
`deathElapsed`, and attachment diagnostics. Player rows include `userId` for pairing a target with demo
death events.

The editor exposes `$.CamEditor.setInspectorMode('follow')` and `$.CamEditor.setAttachmentMenuOpen(true)`
for UI inspection. Runtime evidence is written by `mvm_debug start`: `follow.sample`, `follow.deathhold`,
and `follow.attach.render`.
