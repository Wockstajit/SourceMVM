#pragma once

// The Camera Editor's FOLLOW CAMERA tab, CS2's (CameraEditorFollowInspectorJs.h) in CS:S paint:
//
//   pinned bar  the target line, PRESS LIVE TO SEE IT FOLLOW / FOLLOWING LIVE, and Live / Stop,
//               above the rail so Live works from either card
//   rail        SETUP: CAMERA & TARGET (a green LIVE badge while live); SHOT: FRAMING & TRACKING
//   card 1      CAMERA & TARGET: the camera mode, THIS CAMERA (Place, Reposition, Clear), then the
//               target: type, the weapon's player and source, the target list with Go To, Select
//               Nearest, a far pick's confirmation, the mount, the demo's events
//   card 2      FRAMING & TRACKING, Reset in its header, two pages: FRAMING (offset, rotation, FOV)
//               and TRACKING (aim, on death, when the target disappears)
//
// The model is CS2's follow state (FollowCamera::BuildStateJson), the payload CS2's inspector
// reads; every control sends the command CS2's sends (mirv_filmmaker follow ...). Rows that belong
// to another mode or target type are left out, as CS2 leaves them out. Engine-free: the game reads
// the follow camera into the model and runs the commands; the browser mockup uses a stand-in.

#include "InspectorCards.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

struct FollowInspectorPoint {
	std::string id, label, kind;
	bool valid = false;
};

struct FollowInspectorPlayer {
	int index = -1;
	std::string name, team;
	bool alive = false;
};

struct FollowInspectorCandidate {
	int index = -1;
	uint64_t handle = 0;
	std::string name, className, team, status, grenadeType, ownerName;
	bool alive = false, isWeapon = false, isBomb = false, dropped = false;
	int throwTick = -1, tickDelta = 0;
	double distance = 0;
	int distanceLevel = 0; // 0 near, 1 medium, 2 far, 3 very far
	std::vector<FollowInspectorPoint> attachPoints;
	std::vector<std::string> attachments;
};

struct FollowInspectorEvent {
	int index = -1, tick = 0;
	std::string type, item, ownerName;
};

struct FollowInspectorModel {
	bool enabled = false, hasCamera = false, repositioning = false;
	int type = 0;             // 0 player, 1 grenade, 2 weapon or C4
	std::string typeName;
	int mode = 0;             // 0 lock-on, 1 attached
	int weaponSource = 0;     // 0 auto, 1 held, 2 dropped
	int weaponPlayerIndex = -1, weaponFilterPlayer = -1;
	int targetIndex = -1;
	uint64_t targetHandle = 0;
	bool targetValid = false;
	std::string targetName;
	double targetDistance = 0;
	int distanceLevel = 0;
	double offset[3] = { 0, 0, 0 }, rotation[3] = { 0, 0, 0 };
	double fov = 90, look = 0, position = 0, prediction = 0, deadzone = 0, maxTurn = 0, deathHold = 0;
	bool autoDead = false, switchWeapon = false, switchBomb = false, holdLast = false;
	std::string attachment;
	std::vector<FollowInspectorPoint> attachPoints;
	std::string message;
	bool grenadePending = false, debug = false;
	// Attached's diagnostics (follow debug 1).
	bool attachDebugActive = false;
	int attachDebugEntity = -1;
	std::string attachDebugId, attachDebugSource;
	double attachDebugTarget[3] = { 0, 0, 0 }, attachDebugCamera[3] = { 0, 0, 0 };
	double attachDebugDistance = 0, attachDebugAim = 0, attachDebugJitter = 0, attachDebugCameraDelta = 0, attachDebugTargetDelta = 0;
	std::vector<FollowInspectorPlayer> players;
	std::vector<FollowInspectorCandidate> candidates;
	int eventStatus = 0;      // 0 idle, 1 scanning, 2 ready, 3 failed
	unsigned long long eventScanElapsedMs = 0;
	std::string eventError;
	int selectedEvent = -1;
	std::vector<FollowInspectorEvent> events;
};

// What a dropdown entry does when picked: a command, or (a far target) the confirmation first.
struct FollowDropPick {
	std::string command;
	int farIndex = -1;
	std::string farText;
};

struct FollowInspectorState {
	int card = 0;     // 0 CAMERA & TARGET, 1 FRAMING & TRACKING
	int shotPage = 0; // FRAMING & TRACKING's page: 0 Framing, 1 Tracking
	// The open dropdown (-1 for none), where its field is, and its entries.
	int openDrop = -1;
	Rect dropField;
	std::vector<DropOption> dropOptions;
	std::vector<FollowDropPick> dropPicks;
	// A far target waiting for Use anyway (CS2's pendingCandidate), and what the row says about it.
	int pendingIndex = -1;
	std::string pendingText;
};

// The tab's pinned bar, rail and cards, into the inspector's body (client px). commands get the
// console commands the controls send, in order.
void DrawFollowInspector(FollowInspectorState& state, const FollowInspectorModel& model, const FontSet& fonts, float s,
	const Rect& body, std::vector<std::string>& commands);

// The open dropdown's list, over everything. Draw it after the panels.
void DrawFollowDropList(FollowInspectorState& state, const FontSet& fonts, float s, float clientW, float clientH,
	std::vector<std::string>& commands);

} // namespace Ui
} // namespace SourceFilmmaker
