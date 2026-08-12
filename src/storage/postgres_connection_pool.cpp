#include "storage/postgres_connection_pool.hpp"
#include "storage/postgres_catalog.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {
static atomic<bool> pg_use_connection_cache {true};

PostgresPoolConnection::PostgresPoolConnection() : pool(nullptr) {
}

PostgresPoolConnection::PostgresPoolConnection(optional_ptr<PostgresConnectionPool> pool,
                                               PostgresConnection connection_p)
    : pool(pool), connection(std::move(connection_p)) {
}

PostgresPoolConnection::~PostgresPoolConnection() {
	if (pool) {
		pool->ReturnConnection(std::move(connection));
	}
}

PostgresPoolConnection::PostgresPoolConnection(PostgresPoolConnection &&other) noexcept {
	std::swap(pool, other.pool);
	std::swap(connection, other.connection);
}

PostgresPoolConnection &PostgresPoolConnection::operator=(PostgresPoolConnection &&other) noexcept {
	std::swap(pool, other.pool);
	std::swap(connection, other.connection);
	return *this;
}

bool PostgresPoolConnection::HasConnection() {
	return pool;
}

PostgresConnection &PostgresPoolConnection::GetConnection() {
	if (!HasConnection()) {
		throw InternalException("PostgresPoolConnection::GetConnection called without a transaction pool");
	}
	return connection;
}

PostgresConnectionPool::PostgresConnectionPool(PostgresCatalog &postgres_catalog, idx_t maximum_connections_p)
    : postgres_catalog(postgres_catalog), active_connections(0), maximum_connections(maximum_connections_p) {
}

PostgresPoolConnection PostgresConnectionPool::GetConnectionInternal(unique_lock<mutex> &lock) {
	active_connections++;
	// check if we have any cached connections left
	if (!connection_cache.empty()) {
		auto connection = PostgresPoolConnection(this, std::move(connection_cache.back()));
		connection_cache.pop_back();
		return connection;
	}

	// Reserve the slot before releasing the lock so concurrent callers cannot exceed the limit.
	lock.unlock();
	try {
		return PostgresPoolConnection(this, PostgresConnection::Open(postgres_catalog.connection_string));
	} catch (...) {
		lock.lock();
		active_connections--;
		throw;
	}
}

bool PostgresConnectionPool::TryGetConnection(PostgresPoolConnection &connection) {
	unique_lock<mutex> l(connection_lock);
	if (active_connections >= maximum_connections) {
		return false;
	}
	connection = GetConnectionInternal(l);
	return true;
}

void PostgresConnectionPool::PostgresSetConnectionCache(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw BinderException("Cannot be set to NULL");
	}
	pg_use_connection_cache.store(BooleanValue::Get(parameter));
}

PostgresPoolConnection PostgresConnectionPool::GetConnection() {
	PostgresPoolConnection result;
	if (!TryGetConnection(result)) {
		idx_t active_connection_count;
		idx_t maximum_connection_count;
		{
			lock_guard<mutex> l(connection_lock);
			active_connection_count = active_connections;
			maximum_connection_count = maximum_connections;
		}
		throw IOException(
		    "Failed to get connection from PostgresConnectionPool - maximum connection count exceeded (%llu/%llu max)",
		    active_connection_count, maximum_connection_count);
	}
	return result;
}

void PostgresConnectionPool::ReturnConnection(PostgresConnection connection) {
	unique_lock<mutex> l(connection_lock);
	if (active_connections <= 0) {
		throw InternalException("PostgresConnectionPool::ReturnConnection called but active_connections is 0");
	}
	if (!pg_use_connection_cache.load()) {
		active_connections--;
		return;
	}

	// Session cleanup can block on libpq, so it must not hold the pool lock.
	l.unlock();
	bool connection_is_usable = false;
	try {
		connection_is_usable = connection.Reset();
	} catch (...) {
		connection_is_usable = false;
	}
	l.lock();
	active_connections--;
	if (!connection_is_usable || !pg_use_connection_cache.load()) {
		return;
	}
	if (active_connections >= maximum_connections) {
		// If the limit was lowered while this connection was in use, reclaim it now.
		return;
	}
	connection_cache.push_back(std::move(connection));
}

idx_t PostgresConnectionPool::GetMaximumConnections() {
	lock_guard<mutex> l(connection_lock);
	return maximum_connections;
}

void PostgresConnectionPool::SetMaximumConnections(idx_t new_max) {
	lock_guard<mutex> l(connection_lock);
	if (new_max < maximum_connections) {
		// potentially close connections
		// note that we can only close connections in the connection cache
		// we will have to wait for connections to be returned
		auto total_open_connections = active_connections + connection_cache.size();
		while (!connection_cache.empty() && total_open_connections > new_max) {
			total_open_connections--;
			connection_cache.pop_back();
		}
	}
	maximum_connections = new_max;
}

} // namespace duckdb
