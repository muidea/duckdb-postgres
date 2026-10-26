#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_crud_deadline.hpp"
#include "guarded_ddl_intent.hpp"
#include "storage/postgres_catalog.hpp"

namespace duckdb {
namespace {
struct Data : TableFunctionData {
	PostgresCatalog &catalog;
	GuardedDDLIntent intent;
	Data(PostgresCatalog &c, GuardedDDLIntent d) : catalog(c), intent(std::move(d)) {}
};
struct State : GlobalTableFunctionState {
	bool done = false;
};
static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<State>();
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
									 vector<string> &names) {
	auto intent = GuardedDDLIntent::Bind(in.inputs, false);
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, GuardedDDLIntent::Text(in.inputs[0]));
	if (!db || db->GetCatalog().GetCatalogType() != "postgres")
		throw BinderException("DDL catalog is unavailable");
	names = {"operation_id", "status", "sqlstate", "stage", "warning_count"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
			 LogicalType::BIGINT};
	return make_uniq<Data>(db->GetCatalog().Cast<PostgresCatalog>(), std::move(intent));
}
using Result = unique_ptr<PGresult, decltype(&PQclear)>;
static Result Wrap(PGresult *r) { return Result(r, PQclear); }
static bool OK(PGresult *r) { return r && PQresultStatus(r) == PGRES_COMMAND_OK; }
static string ErrorState(PGresult *r) {
	const char *s = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : nullptr;
	return s && strlen(s) == 5 ? s : "08006";
}
static void Scan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<State>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<Data>();
	string status = "CONFIRMED_NOT_EXECUTED", error, stage = "source_identity";
	PostgresPoolConnection pooled;
	unique_ptr<GuardedCRUDDeadline> deadline;
	bool transaction = false, sent = false, commit_sent = false;
	try {
		if (ctx.IsInterrupted())
			throw InterruptException();
		pooled = d.catalog.GetConnectionPool().GetConnection();
		auto raw = pooled.GetConnection().GetConn();
		deadline = make_uniq<GuardedCRUDDeadline>(ctx, PQsocket(raw), 30);
		if (PQtransactionStatus(raw) != PQTRANS_IDLE)
			throw IOException("DDL session is not idle");
		auto reset = Wrap(PQexec(raw, "DISCARD ALL"));
		if (!OK(reset.get()))
			throw IOException("DDL reset failed");
		auto identity = Wrap(PQexec(raw, "SELECT "
										 "current_database(),version(),current_setting('server_version_num')"));
		if (!identity || PQresultStatus(identity.get()) != PGRES_TUPLES_OK || PQntuples(identity.get()) != 1)
			throw IOException("DDL source identity unavailable");
		string version = PQgetvalue(identity.get(), 0, 1);
		int major = std::stoi(PQgetvalue(identity.get(), 0, 2)) / 10000;
		if (string(PQgetvalue(identity.get(), 0, 0)) != d.intent.database || version.find("PostgreSQL ") != 0 ||
			version.find("Greenplum") != string::npos || (major != 12 && major != 17)) {
			error = "0A000";
			throw IOException("DDL source not certified");
		}
		auto begin = Wrap(PQexec(raw, "BEGIN"));
		if (!OK(begin.get()))
			throw IOException("DDL begin failed");
		transaction = true;
		auto settings = Wrap(PQexec(raw, "SET LOCAL search_path=pg_catalog; SET LOCAL statement_timeout='25s'; "
										 "SET LOCAL lock_timeout='5s'; SET LOCAL "
										 "standard_conforming_strings=on; SET LOCAL client_encoding='UTF8'"));
		if (!OK(settings.get())) {
			error = ErrorState(settings.get());
			throw IOException("DDL settings failed");
		}
		if (ctx.IsInterrupted())
			throw InterruptException();
		stage = "source_execute";
		sent = true;
		auto result = Wrap(PQexec(raw, d.intent.sql.c_str()));
		if (!OK(result.get())) {
			error = ErrorState(result.get());
			throw IOException("DDL execution failed");
		}
		stage = "source_commit";
		commit_sent = true;
		auto commit = Wrap(PQexec(raw, "COMMIT"));
		if (!OK(commit.get()) || string(PQcmdStatus(commit.get())) != "COMMIT" ||
			PQtransactionStatus(raw) != PQTRANS_IDLE) {
			error = ErrorState(commit.get());
			throw IOException("DDL commit outcome unavailable");
		}
		transaction = false;
		status = "COMMITTED";
		stage.clear();
	} catch (...) {
		if (error.empty())
			error = ctx.IsInterrupted() ? "57014" : "08006";
		if (sent) {
			status = "UNKNOWN";
			if (transaction && !commit_sent && pooled.HasConnection()) {
				auto rollback = Wrap(PQexec(pooled.GetConnection().GetConn(), "ROLLBACK"));
				if (OK(rollback.get()))
					status = "CONFIRMED_ROLLED_BACK";
			}
		}
	}
	if (pooled.HasConnection()) {
		deadline.reset();
		pooled.GetConnection().Close();
	}
	out.SetCardinality(1);
	out.SetValue(0, 0, Value(d.intent.operation));
	out.SetValue(1, 0, Value(status));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value::BIGINT(0));
}
} // namespace
void RegisterPostgresGuardedDDL(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction(
		"postgres_guarded_ddl_v2",
		{LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
		Scan, Bind, Init));
}
} // namespace duckdb
