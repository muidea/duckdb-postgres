#pragma once
#include "guarded_source_error.hpp"
#include "query_source_error.hpp"
#include <libpq-fe.h>
namespace duckdb {
inline string PostgresQueryErrorMessage(PGresult *result, PGconn *connection, const string &stage,
                                        const string &query = "") {
	GuardedSourceError e;
	auto field = [&](int code) {
		auto value = result ? PQresultErrorField(result, code) : nullptr;
		return value ? string(value) : string();
	};
	e.product = "postgres";
	e.state = field(PG_DIAG_SQLSTATE);
	e.code = e.state;
	e.message = field(PG_DIAG_MESSAGE_PRIMARY);
	e.detail = field(PG_DIAG_MESSAGE_DETAIL);
	e.hint = field(PG_DIAG_MESSAGE_HINT);
	e.schema = field(PG_DIAG_SCHEMA_NAME);
	e.table = field(PG_DIAG_TABLE_NAME);
	e.column = field(PG_DIAG_COLUMN_NAME);
	e.constraint = field(PG_DIAG_CONSTRAINT_NAME);
	if (e.message.empty()) {
		auto text = result ? PQresultErrorMessage(result) : (connection ? PQerrorMessage(connection) : nullptr);
		e.message = text ? string(text) : "PostgreSQL operation failed without a source diagnostic";
	}
	return QuerySourceMessage(e.state.empty() ? "" : e.JSON(), e.message, stage, query, "postgres");
}
} // namespace duckdb
