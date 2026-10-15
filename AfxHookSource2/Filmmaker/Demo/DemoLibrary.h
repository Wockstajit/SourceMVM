#pragma once

// Owns the discovered demo list and the background scan that builds it.
// Thread-safe: the scan runs on a worker thread; readers take a snapshot.

#include "DemoEntry.h"
#include "../Config/DemoFolderStore.h"
#include "../Config/DemoFileStore.h"
#include "../Config/HiddenDemoStore.h"

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <condition_variable>
#include <unordered_set>

namespace Filmmaker {

class DemoLibrary {
public:
	DemoLibrary() = default;
	~DemoLibrary();

	// Loads saved folders. Does not scan yet.
	void Init();

	// Stops any running scan and joins the worker thread.
	void Shutdown();

	// Kicks off a background (re)scan of the install root + saved folders.
	void StartScan();

	bool IsScanning() const { return m_scanning.load(); }

	// Monotonic counter bumped whenever the entry list changes; lets the UI
	// detect "new results are ready" without polling the whole list.
	uint64_t Version() const { return m_version.load(); }

	// Snapshot of the current entries (thread-safe copy).
	std::vector<DemoEntry> Snapshot() const;

	// Resolves the on-disk path for an entry index in the current list.
	bool PathByIndex(size_t index, std::wstring& outPath) const;

	// Requests the per-round timeline for one demo (a full demo parse) on a
	// background thread, if not already present/queued. Cheap + idempotent; the
	// list version bumps when the data lands so the UI fills in live.
	void EnsureRounds(size_t index);

	// Folder management (auto-saves). Returns true on change.
	//
	// AddFolder also UN-HIDES every demo under `folder` (see HiddenDemoStore): re-adding a
	// folder is the documented way to bring back demos removed with the delete button. It
	// therefore reports true when it un-hid something even if the folder itself was already
	// saved, so the caller still rescans.
	bool AddFolder(const std::wstring& folder);
	bool RemoveFolder(const std::wstring& folder);
	std::vector<std::wstring> Folders() const;

	// Individually added demo FILES (auto-saves). Unlike AddFolder this lists exactly this one
	// demo and nothing else beside it. Like AddFolder it also un-hides the demo, so re-adding a
	// file the user previously removed from the list brings it back.
	//
	// No public reader to match Folders(): these paths are only ever consumed by ScanWorker,
	// which reads m_fileStore directly under the lock. An accessor nothing calls is dead code.
	bool AddDemoFile(const std::wstring& path);

	// Hidden demos ("delete" in the UI). The .dem is never touched on disk; the entry is just
	// dropped from the list the UI renders. Auto-saves.
	// A demo was RENAMED on disk (Filmmaker::RenameDemoFile). Move the two path-keyed pieces of
	// user state that would otherwise be stranded: an individually-added file entry, and the
	// hidden flag. Auto-saves. The entry list itself is not touched -- the caller rescans, which
	// is what makes the new name appear.
	void NoteRenamed(const std::wstring& oldPath, const std::wstring& newPath);

	bool HideDemo(size_t index);          // by list index (what the UI has)
	bool UnhideDemo(const std::wstring& path);
	size_t UnhideAll();
	size_t HideAllVisible();              // "Clear all": hides every currently-listed demo
	std::vector<std::wstring> HiddenDemos() const;

	std::wstring InstallRoot() const;

	// Serializes the current state to a JSON string for the Panorama UI.
	std::string BuildJson() const;

private:
	void ScanWorker();

	// Background per-round enrichment (lazy full parse on selection).
	void EnrichWorker();
	void MergeRoundsForPath(const std::wstring& path);

	// Re-applies m_hiddenStore to m_entries' `hidden` flags. Call with m_mutex held.
	void ApplyHiddenLocked();

	mutable std::mutex m_mutex;
	std::vector<DemoEntry> m_entries;
	DemoFolderStore m_folderStore;
	DemoFileStore m_fileStore;
	HiddenDemoStore m_hiddenStore;

	std::thread m_worker;
	std::atomic<bool> m_scanning{ false };
	std::atomic<bool> m_cancel{ false };
	std::atomic<uint64_t> m_version{ 0 };

	std::thread m_enrichWorker;
	std::mutex m_enrichMutex;
	std::condition_variable m_enrichCv;
	std::deque<std::wstring> m_enrichQueue;
	std::unordered_set<std::wstring> m_enrichRequested;
	bool m_enrichStop = false;
};

} // namespace Filmmaker
