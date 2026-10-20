//===----------------------------------------------------------------------===//
//                         DuckDB
//
// postgres_catalog_cache.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <mutex>

namespace duckdb {

// Counts estimated table-entry payload, including entries still borrowed by a
// transaction after the catalog drops its reference. It is not an RSS bound.
class PostgresCatalogCacheBudget {
public:
	static constexpr uint64_t LIMIT = 16ULL << 20;
	struct Stats {
		uint64_t retained_bytes = 0, live_bytes = 0, live_entries = 0, peak_bytes = 0, uncached_loads = 0;
	};

	bool Acquire(uint64_t bytes) {
		std::lock_guard<std::mutex> guard(lock);
		stats.live_bytes += bytes;
		stats.live_entries++;
		stats.peak_bytes = stats.live_bytes > stats.peak_bytes ? stats.live_bytes : stats.peak_bytes;
		if (bytes > LIMIT || stats.retained_bytes > LIMIT - bytes) {
			stats.uncached_loads++;
			return false;
		}
		stats.retained_bytes += bytes;
		return true;
	}

	void Release(uint64_t bytes, bool retained) {
		std::lock_guard<std::mutex> guard(lock);
		stats.live_bytes -= bytes;
		stats.live_entries--;
		if (retained) {
			stats.retained_bytes -= bytes;
		}
	}

	Stats Read() {
		std::lock_guard<std::mutex> guard(lock);
		return stats;
	}

private:
	std::mutex lock;
	Stats stats;
};

} // namespace duckdb
