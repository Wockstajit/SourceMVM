#pragma once

// Public entry points for the filmmaker demo-browser feature.
// This is the single integration surface the rest of AfxHookSource2 touches.

#include <string>
#include <cstddef>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class DemoLibrary;
class FilmmakerMenu;

// Lazily initialized; safe to call repeatedly.
void EnsureInitialized();

// Called once per rendered frame from the Present hook (RENDER thread).
// Pumps thread-safe backend work only (start scans, apply picked folders).
// Must NOT touch Panorama (V8 lives on the main thread).
void RunFrame();

// Called once per frame from FrameStageNotify (MAIN/UI thread). Drives the
// Panorama menu (RunScript / panel IO), which must run on the main thread.
void RunMainThreadFrame();

// Called from FrameStageNotify (MAIN thread) AFTER the engine's own handler has run, i.e. as
// late in the frame as we can still get before the draw.
//
// Why this exists: the client re-applies each weapon's recorded mesh-group mask from the demo
// snapshot during its own FRAME_RENDER_PASS work. RunMainThreadFrame runs BEFORE that, so a
// legacy-vs-CS2 mesh selection written there was overwritten every frame and the renderer always
// drew the recorded group (proved live: "live=1 wanted=2" on every one of six native attempts,
// while the same-frame read-back showed postMask=4). Re-asserting here makes our write the last
// one before the draw. Mesh-group reassert ONLY -- no Panorama, no econ writes.
void RunMainThreadLateFrame();

// Called after the advancedfx Rust callback queue. Publishes ragdoll draw-probe targets after
// applying the hand pose. Engine scene transforms can still be lazily rebuilt afterward, so
// RagdollWeapon must keep the interpolated source pose coherent with the final transform.
void RunMainThreadPostFrame();

// Releases the worker thread on shutdown.
void Shutdown();

// Actions (callable from the console thread or the UI command queue).
void RequestRescan();
void RequestAddFolder();
// Opens a multi-select .dem file dialog. Lists exactly the chosen files, without pulling in
// everything else sitting next to them (which is what RequestAddFolder would do).
void RequestAddDemo();

// Hide the first-person viewmodel + suppress its _fp weapon FX while the free cam is detached.
// Default OFF so flying the free cam into a player still shows the arms/gun (the standard
// "latched inside them" shot). See the block in RunMainThreadFrame.
void SetFpHideWhenDetached(bool on);
bool FpHideWhenDetached();

// Pump heartbeats, for diagnosing "the demo list is empty and nothing happens". outRenderCalls
// counts Filmmaker::RunFrame() (render thread, via Before_Present); outMainCalls counts
// RunMainThreadFrame(). A render count of 0 means the Present/scenesystem hooks did not install.
void PumpStatus(uint64_t& outRenderCalls, uint64_t& outMainCalls, uint64_t& outScansStarted);
void Watch(std::size_t index);
// Opens the Source MVM project directory for the selected demo (`source_mvm/demos/cs2/<demo>/`).
void OpenDemoProjectFolder(std::size_t index);

// Renames the demo at `index` on disk. Authored state lives in the source_mvm project
// folder's project.mvm and follows via DemoProjectStore_NoteRenamed. A leftover Valve
// .dem.info beside the file is renamed with it. The name is sanitised and forced to end in .dem; an existing target, or a
// demo the engine currently has open, is refused with a message and changes nothing.
void RenameDemoFile(std::size_t index, const std::string& newNameUtf8);
// Starts the one-time compatibility check for a selected legacy demo without starting
// playback. Watch() later reuses the cached clean/repaired verdict.
void PrepareDemo(std::size_t index);

DemoLibrary& Library();
FilmmakerMenu& Menu();

// Absolute path of the demo most recently launched via OUR Watch(), or empty if a
// demo was started another way (e.g. console playdemo or the native Your Matches tab).
// Returns a copy (thread-safe). Prefer PlayingDemoPath() for the camera-path sidecar.
std::wstring CurrentDemoPath();

// Canonical logical project path of the demo the ENGINE is actually playing, irrespective of
// how it started (our Downloaded tab, native Your Matches, or console playdemo). A repaired
// cache copy with a valid source marker maps back to the original .dem so both copies share
// the same source_mvm project folder. Falls back to CurrentDemoPath() (canonicalized) when the
// engine path can't be recovered. Cheap to call per frame.
std::wstring PlayingDemoPath();

// Reopens the playing demo on its newest finished scrub keyframe copy, at the next quiet pause,
// landing back on the same tick (`mirv_filmmaker keyframes switch`). Never automatic: a copy that
// finishes mid-session is otherwise used from the next open. False when there is nothing to switch to.
bool DemoKeyframes_SwitchNow();

// --- Movie director (camera modes + in-game help HUD) ---
// Input taps, called from the WndProc / input thread. Return true if the event
// was consumed (the caller should then swallow it). Safe no-ops if unused.
bool MovieInput_OnKey(int vkey, bool down);
bool MovieInput_OnMouseButton(int button, bool down, int x, int y); // 0 = left, 1 = right; client px
void MovieInput_ButtonUp(int button); // raw-input release, 0 = left, 1 = right (MovieMode::OnButtonUp)
// Left press at client (x, y): true = a game-mode click on our UI, now switching to UI mode; the
// caller passes the press and its release straight to the game window (MovieMode::OnUiClick).
bool MovieInput_OnUiClick(int x, int y);
bool MovieInput_OnMouseWheel(int delta, bool shiftDown, bool ctrlDown, int x = -1, int y = -1);

// HUD panel show/hide (driven by the mirv_filmmaker hud console command).
void MovieHud_Set(bool visible);
void MovieHud_Toggle();
bool MovieHud_Visible();
void MovieHud_SetReadoutRate(unsigned hz);
unsigned MovieHud_ReadoutRate();

// Queues Panorama JS to run in the HUD-panel context (mirv_filmmaker hud_eval).
void MovieHud_Eval(const std::string& js);

// Native demo-bar inline speed buttons (mirv_filmmaker speedbar). When off, the
// native timescale dropdown is restored.
void DemoSpeedBar_Set(bool enabled);
void DemoSpeedBar_Toggle();
bool DemoSpeedBar_Enabled();

// True while the BO2-style camera-marker SETTINGS menu is open (Editing mode).
// main.cpp ORs this into GetSuspendMirvInput() so the OS cursor is shown and
// free-cam mouse-look is suspended while the menu is up (exactly like the
// console-open case), making the menu's Panorama buttons clickable. Free cam
// itself stays enabled, so the camera holds its pose and look resumes when the
// menu closes. Cheap; safe to call every frame from the input/render path.
bool MarkerMenu_WantsCursor();

// True while filmmaker cursor mode owns the mouse. The camera timeline / curve
// editor forces this on while open; otherwise the regular native-bar/G cursor
// toggle controls it in third-person/freecam.
bool CameraTimeline_WantsCursor();
// Any thread: whether a client-px point is on CS2's native demo bar (scrubber, CAM EDITOR, CONFIG).
bool CameraTimeline_DemoBarContains(int x, int y);

// True while the camera timeline panel is open (any cursor mode). Read by the
// input layer to decide whether G should toggle the panel's UI-mouse mode.
bool CameraTimeline_Visible();
// The per-keyframe menu (right-click a diamond) is modal for the WHEEL: while it is up its
// style list takes the notches instead of the workspace behind it.
bool CameraTimeline_MenuOpen();
void CameraTimeline_PushWheel(int notches); // +1 = wheel up
// CS2's demo-bar camera button: 0 first person, 1 chase, 2 free cam, -1 unknown. Main thread.
int CameraTimeline_NativeCameraMode();
// CS2's own spectator state, read off the native demo bar's panel (CameraTimelineHud.h): the
// spectated pawn (-1 none), the observer mode (2 first, 3 chase, 4 roaming, -1 unknown), the player
// list rows, and a request to spectate a player controller the way CS2's player list does.
int CameraTimeline_NativeSpectatedPawn();
int CameraTimeline_NativeObserverMode();
const std::string& CameraTimeline_NativeDemoPlayers();
void CameraTimeline_RequestNativeSpectate(int controllerIndex);

// True while the offline mvm_test FX menu is open in UI-cursor mode. main.cpp's
// CCSGOInput::MouseInputEnabled detour returns false while this holds so a LIVE match
// releases the mouse to the cursor (see the .cpp comment for why suspend alone can't).
bool LiveFxMenu_WantsGameMouseReleased();

// True while the camera PATH is actively driving the final view (dolly playing + free cam
// engaged). main.cpp's view-setup hook uses this to block CS2's demo-view-override
// (TrueView) from re-owning the view each frame the demo advances. CampathDebug() gates the
// verbose [campath]/[setupview] instrumentation (toggle via "camtl debug 0|1").
bool CameraPathOwnsView();
bool CampathDebug();

// --- Camera Editor Mode (dedicated editor workspace) ---
// Toggleable workspace that frames the live game as a preview and surrounds it with the
// camera inspector + timeline. Driven by "mirv_filmmaker editor on|off|toggle".
void CameraEditor_Set(bool enabled);
bool CameraEditor_Enabled();
// Pulse the inspector so the user's eye is pulled to it. Used when F selects an aimed-at camera
// in the editor: the settings for that camera are already on the right, so the job is to say
// "they are over here", not to open a second editor on top of the workspace.
void CameraEditor_FlashInspector();
void CameraEditor_Toggle();
// True while Camera Editor Mode is active. Read by the input layer so G keeps toggling
// the UI/GAME mouse (to fly the free cam) even though the timeline panel is open.
bool CameraEditor_Active();
// Switch Camera Editor between UI cursor ownership and game/free-cam mouse look.
// Also pauses/resumes graph-editor view driving so mouse look is not overwritten.
void CameraEditor_SetCursorMode(bool uiCursor);

// TRUE scaled preview viewport (render-layer): scales the whole rendered frame into the
// preview rect instead of showing a crop. Driven by "mirv_filmmaker editor scale on|off|
// toggle"; only takes visible effect while the editor is open and not recording.
void CameraEditor_SetScale(bool enabled);
void CameraEditor_ToggleScale();
bool CameraEditor_ScaleActive();

// Put the shared dock back on CS2's own demo bar (REGULAR TIMELINE).
void CameraEditor_SetNativeTimeline();

// Game-HUD visibility while the editor is open ("hidden" | "game" | "full"). Read by
// CameraTimelineHud's state push so its JS shows/hides the native gameplay HUD accordingly.
const char* CameraEditor_HudViewName();

// Scaled-preview rect (normalised window fractions) the world blit uses; returns true when the
// scaled viewport is live. The timeline HUD scales the native game HUD into this same rect.
bool CameraEditor_ScaledHud(float& x0, float& y0, float& x1, float& y1);

// True while the Camera Editor's "Customize Player" modal is open (custOverlay.visible, published
// JS -> C++ via the "customizeopen" panel attribute, same pattern as "previewrect"). The modal is
// meant to be a full-screen EXCLUSIVE surface: MovieMode's key/mouse/wheel handlers swallow input
// unconditionally while this is true, and main.cpp's GetSuspendMirvInput() ORs it in so free-cam
// mouse-look can't compete with dragging/scrolling inside the modal.
bool CameraEditor_CustomizeModalOpen();
// CONFIG's CUSTOMIZE PLAYER: open the same modal over CONFIG with the editor workspace closed.
void CameraEditor_OpenCustomizeOverConfig();
// The Camera Editor's Editor Settings fly-out is up (its overlay takes every click).
bool CameraEditor_PopupOpen();

// Route captured input into the Customize modal's Panorama JS. In the in-game HUD context CS2 keeps
// keyboard/wheel priority (a Panorama TextEntry never gets real focus, and the wheel still drives
// the demo spectator), so the modal cannot rely on native input the way a main-menu popup can.
// Instead MovieMode/main.cpp SWALLOW the raw wheel + typed characters from the game while the modal
// is open and forward them here; CameraEditorHud drains these on the main thread and dispatches them
// to $.CamEditor.custWheel/custChars so the modal scrolls + its search boxes type -- with nothing
// leaking to the demo behind it. Thread-safe (called from the WndProc/input thread).
void CameraEditor_CustomizePushWheel(int notches, int x, int y); // +1 = wheel up, -1 = wheel down; client coords
void CameraEditor_CustomizePushChar(unsigned charCode); // WM_CHAR code unit (incl. 8 backspace, 27 esc)
// True while a filmmaker text field (Render workspace FPS / resolution box) is collecting typed
// characters. The WM_CHAR hook routes into the same queue as the Customize search boxes, and the
// key handler must swallow the keystroke so typing a number does not also drive the demo.
bool CameraEditor_TextCaptureActive();

// --- Experimental After-Effects-style graph editor (isolated, opt-in overlay) ---
// Default OFF; toggled by the "Experiment" button or "mirv_filmmaker grapheditor on|off|
// toggle". OwnsView()/WantsCursor() are OR'd into the stable view-ownership + cursor-routing
// hooks so the experiment can live-drive the camera and stay clickable while open. Deleting
// the GraphEditor* files + reverting these hooks removes the feature entirely.
void GraphEditorExperiment_Set(bool enabled);
void GraphEditorExperiment_Toggle();
bool GraphEditorExperiment_Enabled();
bool GraphEditorExperiment_OwnsView();
// The TIMELINE dock's CAMERA PATH layer switch: may scrubbing and Space fly the camera path.
bool GraphEditorExperiment_Drive();
bool GraphEditorExperiment_WantsCursor();

// --- mvm_test offline live-match FX testing ---
bool MvmTest_Active();
bool MvmTest_CanUseMenu();

// --- Fisheye lens (standalone barrel-distortion D3D pass, GoPro-style look) ---
bool Fisheye_Enabled();
void Fisheye_SetEnabled(bool on);
void Fisheye_Toggle();
void Fisheye_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// True when non-preview UI should be hidden: always during full path playback,
// and while the armed preview has its background HUD toggled off with Tab.
bool CameraPath_PreviewHudHidden();

} // namespace Filmmaker
