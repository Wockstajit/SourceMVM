#include "FollowEventIndex.h"

#include "FollowCameraHost.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>
#include <algorithm>
#include <mutex>
#include <thread>

namespace Filmmaker {

void FollowCamera::EnsureEventsLoaded() const {
	const std::string path = FollowHost_DemoPath();
	if (path.empty()) return;

	{
		std::lock_guard<std::mutex> lock(m_eventMutex);
		const int status = m_eventStatus.load();
		if (m_eventDemoPath == path && status == (int)EventStatus::Ready) return;
		if (m_eventDemoPath == path && status == (int)EventStatus::Failed) return;
		if (m_eventLoadingPath == path && status == (int)EventStatus::Loading) return;

		m_eventLoadingPath = path;
		m_eventDemoPath.clear();
		m_events.clear();
		m_eventError.clear();
		m_eventHelperPath.clear();
		m_eventScanStartMs = GetTickCount64();
		m_eventStatus.store((int)EventStatus::Loading);
	}

	// Join any previous worker before starting a new one (done OUTSIDE m_eventMutex so the
	// old worker's final commit, which takes that lock, can't deadlock the join). Cancelling
	// aborts its in-flight scan promptly so the join is bounded.
	if (m_eventThread.joinable()) {
		m_eventCancel.store(true);
		m_eventThread.join();
	}
	m_eventCancel.store(false);

	// Full scan on a joinable worker (multi-second); the demo path is keyed so a demo change
	// relaunches. The game's scan honors m_eventCancel so Shutdown() can stop it.
	m_eventThread = std::thread([this, path]() {
		std::vector<FollowEventRecord> evs;
		std::string helperPath, error;
		const bool ok = FollowHost_ReadDemoEvents(path, &m_eventCancel, evs, helperPath, error);
		if (m_eventCancel.load()) return; // shutdown / superseded: don't touch shared state
		std::lock_guard<std::mutex> lk(m_eventMutex);
		if (m_eventLoadingPath == path) {
			m_events = std::move(evs);
			m_eventDemoPath = path;
			m_eventHelperPath = helperPath;
			m_eventError = ok ? std::string() : (error.empty() ? "demo event scan failed" : error);
			m_eventScanStartMs = 0;
			m_eventStatus.store(ok ? (int)EventStatus::Ready : (int)EventStatus::Failed);
		}
	});
}

void FollowCamera::Shutdown() {
	m_eventCancel.store(true);
	if (m_eventThread.joinable())
		m_eventThread.join();
}

std::vector<FollowEventRecord> FollowCamera::EventsSnapshot() const {
	std::lock_guard<std::mutex> lock(m_eventMutex);
	return m_events;
}

bool FollowCamera::SelectEvent(int eventIndex) {
	auto events = EventsSnapshot();
	if (eventIndex < 0 || eventIndex >= (int)events.size()) {
		m_selectedEvent = -1;
		return false;
	}
	m_selectedEvent = eventIndex;
	const auto& e = events[eventIndex];
	m_lastMessage = "Event: " + e.type + " @ tick " + std::to_string(e.tick)
		+ (e.item.empty() ? "" : (" (" + e.item + ")")) + ".";
	return true;
}

bool FollowCamera::PreviewTick() {
	// Grenades use the throw-tick state machine (seek + reacquire the projectile).
	if (m_state.targetType == FollowTargetType::Grenade)
		return TrackSelectedGrenade();

	auto events = EventsSnapshot();
	if (m_selectedEvent < 0 || m_selectedEvent >= (int)events.size()) {
		advancedfx::Warning("[followcam] preview tick: select a recorded event first.\n");
		m_lastMessage = "Select a drop/pickup event first.";
		return false;
	}
	const auto& e = events[m_selectedEvent];
	const int seekTick = (std::max)(0, e.tick - 40);
	// A drop leaves the weapon owner-less, so default to the dropped entity; the live
	// candidate list around this tick then exposes it for selection / attach.
	if (e.type == "weapon_drop" || e.type == "bomb_dropped")
		m_state.weaponSource = WeaponSource::Dropped;
	FollowHost_SeekDemo(seekTick);
	m_lastMessage = "Seeking to tick " + std::to_string(seekTick) + " ("
		+ e.type + (e.ownerName.empty() ? "" : (" - " + e.ownerName)) + ").";
	advancedfx::Message("[followcam] preview tick: %s item=%s seek=%d.\n",
		e.type.c_str(), e.item.c_str(), seekTick);
	return true;
}

} // namespace Filmmaker
