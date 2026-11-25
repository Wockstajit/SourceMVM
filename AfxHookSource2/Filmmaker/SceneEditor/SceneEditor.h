#pragma once

// Standalone Scene Editor workspace for staging Takes, Actor Animations, and Scene Props
// with 3D world floor paths, spatial transform gizmos, multi-track timeline, and curve graph.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker::SceneEditor {

void RunCommand(int argc, advancedfx::ICommandArgs *args, const char *cmd);
void RunMainThreadFrame();
void Shutdown();

bool Enabled();
void SetEnabled(bool enabled);
void Toggle();

bool WantsCursor();
void SetCursor(bool cursor);
void ToggleCursor();
bool FlyCam();
void SetFlyCam(bool fly);

void OnRecordingFinished(int newTrackIdx);

std::string BuildStateJson();
void CancelActiveEdit();
bool BusyEditing();
bool TakeCatalogRequested();

} // namespace Filmmaker::SceneEditor
