#include "FollowEventIndex.h"

#include "FollowTargetProviders.h"
#include "../Demo/DemoInfoHelper.h"
#include "../Platform/TextEncoding.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

void FollowCamera::EnsureEventsLoaded() const {
	const char* p = g_pEngineToClient ? g_pEngineToClient->GetDemoFilePath() : nullptr;
	std::string path = p ? p : "";
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
	// aborts its in-flight helper process promptly so the join is bounded.
	if (m_eventThread.joinable()) {
		m_eventCancel.store(true);
		m_eventThread.join();
	}
	m_eventCancel.store(false);

	// Full parse on a joinable worker (multi-second); the demo path is keyed so a demo
	// change relaunches. ReadDemoInfoViaHelper reuses/refreshes the "<demo>.fmjson" cache
	// and honors m_eventCancel so Shutdown() can stop it.
	m_eventThread = std::thread([this, path]() {
		const std::wstring wpath = Utf8ToWide(path);
		DemoHelperResult result = ReadDemoInfoViaHelper(wpath, {}, &m_eventCancel);
		if (m_eventCancel.load()) return; // shutdown / superseded: don't touch shared state
		std::unordered_map<uint32_t, std::string> names;
		for (const auto& pl : result.players) names[pl.accountId] = pl.name;
		std::vector<FollowEventRecord> evs;
		evs.reserve(result.events.size());
		for (const auto& e : result.events) {
			FollowEventRecord f;
			f.tick = e.tick; f.type = e.type; f.item = e.item; f.accountId = e.accountId;
			auto it = names.find(e.accountId);
			f.ownerName = (it != names.end()) ? it->second : std::string();
			evs.push_back(std::move(f));
		}
		std::lock_guard<std::mutex> lk(m_eventMutex);
		if (m_eventLoadingPath == path) {
			m_events = std::move(evs);
			m_eventDemoPath = path;
			m_eventHelperPath = result.helperPath;
			m_eventError = result.ok ? std::string() : (result.error.empty() ? "demo event scan failed" : result.error);
			m_eventScanStartMs = 0;
			m_eventStatus.store(result.ok ? (int)EventStatus::Ready : (int)EventStatus::Failed);
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
	if (g_pEngineToClient) {
		std::ostringstream cmd; cmd << "demo_gototick " << seekTick;
		g_pEngineToClient->ExecuteClientCmd(0, cmd.str().c_str(), true);
	}
	m_lastMessage = "Seeking to tick " + std::to_string(seekTick) + " ("
		+ e.type + (e.ownerName.empty() ? "" : (" - " + e.ownerName)) + ").";
	advancedfx::Message("[followcam] preview tick: %s item=%s seek=%d.\n",
		e.type.c_str(), e.item.c_str(), seekTick);
	return true;
}

} // namespace Filmmaker
