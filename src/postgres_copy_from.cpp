#include "postgres_binary_reader.hpp"
#include "postgres_connection.hpp"
#include "postgres_source_error.hpp"

namespace duckdb {

void PostgresConnection::BeginCopyFrom(const string &query, ExecStatusType expected_result) {
	PostgresResult pg_res(PQExecute(query.c_str()));
	auto result = pg_res.res;
	if (!result || PQresultStatus(result) != expected_result) {
		throw IOException("%s", PostgresQueryErrorMessage(result, GetConn(), "source_execute", query));
	}
}

} // namespace duckdb
