#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Filmmaker {

// Lock-free publication from the main-thread material scan to the render-thread draw hook. A draw
// is owned by the material path only when that exact live material exposed g_vColorTint during the
// latest scan. Everything else is eligible for the per-draw fallback.
class MaterialTintOwnership {
public:
	void BeginUpdate() {
		m_updateTable = 1u - m_activeTable.load(std::memory_order_acquire);
		for (std::size_t i = 0; i < kCapacity; ++i)
			m_tables[m_updateTable][i].store(0, std::memory_order_relaxed);
	}

	bool Add(const char* materialName) {
		const std::uint64_t hash = Hash(materialName);
		if (!hash) return false;
		std::size_t slot = static_cast<std::size_t>(hash) & (kCapacity - 1);
		for (std::size_t probe = 0; probe < kCapacity; ++probe) {
			std::atomic<std::uint64_t>& entry = m_tables[m_updateTable][slot];
			const std::uint64_t current = entry.load(std::memory_order_relaxed);
			if (current == hash) return true;
			if (current == 0) {
				entry.store(hash, std::memory_order_relaxed);
				return true;
			}
			slot = (slot + 1) & (kCapacity - 1);
		}
		return false;
	}

	void Publish() {
		m_activeTable.store(m_updateTable, std::memory_order_release);
	}

	bool Owns(const char* materialName) const {
		const std::uint64_t hash = Hash(materialName);
		if (!hash) return false;
		const unsigned table = m_activeTable.load(std::memory_order_acquire);
		std::size_t slot = static_cast<std::size_t>(hash) & (kCapacity - 1);
		for (std::size_t probe = 0; probe < kCapacity; ++probe) {
			const std::uint64_t current = m_tables[table][slot].load(std::memory_order_relaxed);
			if (current == hash) return true;
			if (current == 0) return false;
			slot = (slot + 1) & (kCapacity - 1);
		}
		return false;
	}

private:
	static constexpr std::size_t kCapacity = 8192;
	static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

	static std::uint64_t Hash(const char* text) {
		if (!text || !*text) return 0;
		std::uint64_t hash = 14695981039346656037ull;
		for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
			hash ^= *p;
			hash *= 1099511628211ull;
		}
		return hash ? hash : 1;
	}

	std::atomic<std::uint64_t> m_tables[2][kCapacity]{};
	std::atomic<unsigned> m_activeTable{ 0 };
	unsigned m_updateTable = 1;
};

} // namespace Filmmaker
