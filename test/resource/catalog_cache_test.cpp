#include "postgres_catalog_cache.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using duckdb::PostgresCatalogCacheBudget;

static void Require(bool value) {
	if (!value) {
		std::cerr << "catalog cache invariant failed\n";
		std::exit(1);
	}
}

struct Entry {
	std::shared_ptr<PostgresCatalogCacheBudget> budget;
	uint64_t bytes;
	bool retained;
	Entry(std::shared_ptr<PostgresCatalogCacheBudget> budget, uint64_t bytes)
	    : budget(budget), bytes(bytes), retained(budget->Acquire(bytes)) {
	}
	~Entry() {
		budget->Release(bytes, retained);
	}
};

int main() {
	auto budget = std::make_shared<PostgresCatalogCacheBudget>();
	auto full = std::make_shared<Entry>(budget, uint64_t(PostgresCatalogCacheBudget::LIMIT));
	Require(full->retained);
	auto borrowed = full;
	full.reset();
	Require(budget->Read().retained_bytes == PostgresCatalogCacheBudget::LIMIT);
	{
		Entry over(budget, 1);
		Require(!over.retained);
		Require(budget->Read().live_bytes == PostgresCatalogCacheBudget::LIMIT + 1);
	}
	borrowed.reset();
	Require(budget->Read().live_bytes == 0 && budget->Read().retained_bytes == 0);
	{
		Entry oversized(budget, PostgresCatalogCacheBudget::LIMIT + 1);
		Require(!oversized.retained);
	}
	std::vector<std::thread> workers;
	for (int n = 0; n < 8; n++) {
		workers.emplace_back([&]() {
			for (int i = 0; i < 10000; i++) {
				Entry entry(budget, 4ULL << 20);
				Require(budget->Read().retained_bytes <= PostgresCatalogCacheBudget::LIMIT);
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	auto stats = budget->Read();
	Require(stats.live_bytes == 0 && stats.retained_bytes == 0 && stats.live_entries == 0);
	Require(stats.uncached_loads >= 2 && stats.peak_bytes >= PostgresCatalogCacheBudget::LIMIT + 1);
	std::cout << "budget, borrowed lifetime, oversize, 80000 concurrent lifecycles passed\n";
}
