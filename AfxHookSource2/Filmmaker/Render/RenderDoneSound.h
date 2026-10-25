#pragma once

// The chime a render job plays when it ends (Render workspace header: the bell toggle and its
// volume). Synthesised in memory and played through its own XAudio2 voice, so it needs no sound
// file, is heard whatever CS2's own volume or mute is, and never touches the game's audio. A
// finished job rises, a failed one falls, and a cancel (the user's own click) stays silent.
// Settings persist in filmmaker_settings.json.

namespace Filmmaker {

void RenderDoneSound_SetEnabled(bool enabled);
bool RenderDoneSound_Enabled();
// 0..100 percent.
void RenderDoneSound_SetVolume(int percent);
int RenderDoneSound_Volume();

// Plays the chime now when enabled (asynchronous; a new chime cuts the previous one).
void RenderDoneSound_Play(bool finished);
// The same chime whatever the switch says: the header's preview when the volume changes.
void RenderDoneSound_Preview();

void RenderDoneSound_Shutdown();

} // namespace Filmmaker
