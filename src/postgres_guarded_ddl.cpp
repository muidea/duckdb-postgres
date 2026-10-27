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
  Data(PostgresCatalog &c, GuardedDDLIntent d)
      : catalog(c), intent(std::move(d)) {}
};
struct State : GlobalTableFunctionState {
  bool done = false;
};
static unique_ptr<GlobalTableFunctionState> Init(ClientContext &,
                                                 TableFunctionInitInput &) {
  return make_uniq<State>();
}
static unique_ptr<FunctionData> BindContract(ClientContext &ctx,
                                             TableFunctionBindInput &in,
                                             vector<LogicalType> &types,
                                             vector<string> &names,
                                             int32_t contract, bool scoped = false) {
  auto inputs = in.inputs;
  string owner;
  if (scoped) {
    if (inputs.size() != 5 || inputs[4].IsNull() || inputs[4].type().id() != LogicalTypeId::STRUCT)
      throw BinderException("DDL owner scope is unavailable");
    const auto &types = StructType::GetChildTypes(inputs[4].type());
    const auto &fields = StructValue::GetChildren(inputs[4]);
    if (types.size() < 6 || types[5].first != "owner")
      throw BinderException("DDL owner scope is unavailable");
    owner = GuardedDDLIntent::Text(fields[5]);
    if (!GuardedDDLIntent::Name(owner))
      throw BinderException("DDL owner scope is unavailable");
    child_list_t<Value> values;
    for (idx_t i = 0; i < types.size(); i++)
      values.emplace_back(types[i].first, i == 5 ? Value("") : fields[i]);
    inputs[4] = Value::STRUCT(std::move(values));
  }
  auto intent = GuardedDDLIntent::Bind(inputs, false, contract);
  if (scoped) {
    if (intent.action != "drop_index" && intent.action != "rename_index" && intent.action != "reindex")
      throw BinderException("DDL owner scope is unavailable");
    intent.owner = owner;
  }
  auto db = DatabaseManager::Get(ctx).GetDatabase(
      ctx, GuardedDDLIntent::Text(in.inputs[0]));
  if (!db || db->GetCatalog().GetCatalogType() != "postgres")
    throw BinderException("DDL catalog is unavailable");
  names = {"operation_id", "status", "sqlstate", "stage", "warning_count"};
  types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
           LogicalType::VARCHAR, LogicalType::BIGINT};
  return make_uniq<Data>(db->GetCatalog().Cast<PostgresCatalog>(),
                         std::move(intent));
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx,
                                     TableFunctionBindInput &in,
                                     vector<LogicalType> &types,
                                     vector<string> &names) {
  return BindContract(ctx, in, types, names, 2);
}
static unique_ptr<FunctionData> BindV3(ClientContext &ctx,
                                       TableFunctionBindInput &in,
                                       vector<LogicalType> &types,
                                       vector<string> &names) {
  return BindContract(ctx, in, types, names, 3);
}
static unique_ptr<FunctionData> BindV4(ClientContext &ctx,
                                       TableFunctionBindInput &in,
                                       vector<LogicalType> &types,
                                       vector<string> &names) {
  return BindContract(ctx, in, types, names, 4);
}
static unique_ptr<FunctionData> BindScopedV2(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
  return BindContract(ctx, in, types, names, 2, true);
}
static unique_ptr<FunctionData> BindScopedV3(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
  return BindContract(ctx, in, types, names, 3, true);
}
static unique_ptr<FunctionData> BindScopedV4(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
  return BindContract(ctx, in, types, names, 4, true);
}
using Result = unique_ptr<PGresult, decltype(&PQclear)>;
static Result Wrap(PGresult *r) { return Result(r, PQclear); }
static bool OK(PGresult *r) {
  return r && PQresultStatus(r) == PGRES_COMMAND_OK;
}
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
    deadline =
        make_uniq<GuardedCRUDDeadline>(ctx, PQsocket(raw),
                                       d.intent.action == "reindex"     ? 300
                                       : d.intent.contract_version == 4 ? 120
                                                                        : 30);
    if (PQtransactionStatus(raw) != PQTRANS_IDLE)
      throw IOException("DDL session is not idle");
    auto reset = Wrap(PQexec(raw, "DISCARD ALL"));
    if (!OK(reset.get()))
      throw IOException("DDL reset failed");
    auto identity = Wrap(PQexec(
        raw,
        "SELECT "
        "current_database(),version(),current_setting('server_version_num')"));
    if (!identity || PQresultStatus(identity.get()) != PGRES_TUPLES_OK ||
        PQntuples(identity.get()) != 1)
      throw IOException("DDL source identity unavailable");
    string version = PQgetvalue(identity.get(), 0, 1);
    int major = std::stoi(PQgetvalue(identity.get(), 0, 2)) / 10000;
    if (string(PQgetvalue(identity.get(), 0, 0)) != d.intent.database ||
        version.find("PostgreSQL ") != 0 ||
        version.find("Greenplum") != string::npos ||
        (major != 12 && major != 17)) {
      error = "0A000";
      throw IOException("DDL source not certified");
    }
    auto begin = Wrap(PQexec(raw, d.intent.owner.empty() ? "BEGIN" : "BEGIN ISOLATION LEVEL READ COMMITTED"));
    if (!OK(begin.get()))
      throw IOException("DDL begin failed");
    transaction = true;
    const string timeout = d.intent.action == "reindex"     ? "290s"
                           : d.intent.contract_version == 4 ? "110s"
                                                            : "25s";
    auto settingsSQL =
        "SET LOCAL search_path=pg_catalog; SET LOCAL statement_timeout='" +
        timeout +
        "'; "
        "SET LOCAL lock_timeout='5s'; SET LOCAL "
        "standard_conforming_strings=on; SET LOCAL client_encoding='UTF8'";
    auto settings = Wrap(PQexec(raw, settingsSQL.c_str()));
    if (!OK(settings.get())) {
      error = ErrorState(settings.get());
      throw IOException("DDL settings failed");
    }
    if (ctx.IsInterrupted())
      throw InterruptException();
    string scoped_index_oid;
    if (!d.intent.owner.empty()) {
      // The persisted application scope is authority only after acquiring the
      // expected table's lock and checking index ownership on this exact source
      // transaction. A renamed/replaced index cannot escape that table scope.
      const auto target = KeywordHelper::WriteQuoted(d.intent.schema, '"') + "." +
                          KeywordHelper::WriteQuoted(d.intent.owner, '"');
      auto lock = Wrap(PQexec(raw, ("LOCK TABLE ONLY " + target + " IN ACCESS EXCLUSIVE MODE").c_str()));
      if (!OK(lock.get())) {
        error = ErrorState(lock.get());
        throw IOException("DDL owner lock unavailable");
      }
      const char *values[] = {d.intent.schema.c_str(), d.intent.name.c_str(), d.intent.owner.c_str()};
      auto owner = Wrap(PQexecParams(raw,
          "SELECT i.oid::text FROM pg_catalog.pg_class i "
          "JOIN pg_catalog.pg_namespace n ON n.oid=i.relnamespace "
          "JOIN pg_catalog.pg_index x ON x.indexrelid=i.oid "
          "JOIN pg_catalog.pg_class t ON t.oid=x.indrelid "
          "WHERE n.nspname=$1 AND i.relname=$2 AND i.relkind='i' "
          "AND t.relnamespace=n.oid AND t.relname=$3 AND t.relkind='r' "
          "AND t.relpersistence='p' AND NOT t.relispartition "
          "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_inherits h "
          "WHERE h.inhrelid=t.oid OR h.inhparent=t.oid)",
          3, nullptr, values, nullptr, nullptr, 0));
      if (!owner || PQresultStatus(owner.get()) != PGRES_TUPLES_OK || PQntuples(owner.get()) != 1) {
        error = "0A000";
        throw IOException("DDL index owner changed");
      }
      scoped_index_oid = PQgetvalue(owner.get(), 0, 0);
    }
    if (d.intent.contract_version >= 3) {
      // Read-only scope check; no extra source object lock. EXECUTION_NAME
      // still applies to the subsequent native utility statement.
      const char *values[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
      const string kind =
          (d.intent.action == "rename_index" || d.intent.action == "reindex")
              ? "i"
              : "r";
      auto shape = Wrap(
          PQexecParams(raw,
                       ("SELECT 1 FROM pg_catalog.pg_class c JOIN "
                        "pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
                        "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='" +
                        kind +
                        "' AND c.relpersistence='p' "
                        "AND NOT c.relispartition AND NOT EXISTS (SELECT 1 "
                        "FROM pg_catalog.pg_inherits h WHERE h.inhrelid=c.oid "
                        "OR h.inhparent=c.oid)")
                           .c_str(),
                       2, nullptr, values, nullptr, nullptr, 0));
      if (!shape || PQresultStatus(shape.get()) != PGRES_TUPLES_OK) {
        error = "0A000";
        throw IOException("DDL ordinary object scope is unavailable");
      }
      if (PQntuples(shape.get()) != 1) {
        auto present = Wrap(PQexecParams(
            raw,
            "SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace "
            "n ON n.oid=c.relnamespace WHERE n.nspname=$1 AND c.relname=$2",
            2, nullptr, values, nullptr, nullptr, 0));
        if (!present || PQresultStatus(present.get()) != PGRES_TUPLES_OK ||
            PQntuples(present.get()) != 0) {
          error = "0A000";
          throw IOException("DDL ordinary object scope is unavailable");
        }
        // Absence is not a capability failure. Let the one native DDL
        // report its own missing-object/namespace SQLSTATE.
      }
    }
    stage = "source_execute";
    sent = true;
    auto result = Wrap(PQexec(raw, d.intent.sql.c_str()));
    if (!OK(result.get())) {
      error = ErrorState(result.get());
      throw IOException("DDL execution failed");
    }
    if (!scoped_index_oid.empty()) {
      // A table lock alone does not prevent ALTER INDEX RENAME. The native
      // utility holds its index lock until commit. Prove after execution that
      // it acted on the exact observed index; otherwise roll back, never commit
      // a concurrent same-name replacement on a different table.
      const string after_name = d.intent.action == "rename_index" ? d.intent.new_name : d.intent.name;
      const char *values[] = {scoped_index_oid.c_str(), d.intent.schema.c_str(), after_name.c_str(), d.intent.owner.c_str()};
      const char *sql = d.intent.action == "drop_index"
          ? "SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class WHERE oid=$1::oid)"
          : "SELECT 1 FROM pg_catalog.pg_class i "
            "JOIN pg_catalog.pg_namespace n ON n.oid=i.relnamespace "
            "JOIN pg_catalog.pg_index x ON x.indexrelid=i.oid "
            "JOIN pg_catalog.pg_class t ON t.oid=x.indrelid "
            "WHERE i.oid=$1::oid AND n.nspname=$2 AND i.relname=$3 "
            "AND t.relnamespace=n.oid AND t.relname=$4";
      auto verified = Wrap(PQexecParams(raw, sql, d.intent.action == "drop_index" ? 1 : 4,
                                        nullptr, values, nullptr, nullptr, 0));
      if (!verified || PQresultStatus(verified.get()) != PGRES_TUPLES_OK || PQntuples(verified.get()) != 1) {
        error = "0A000";
        throw IOException("DDL index execution scope changed");
      }
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
        auto rollback =
            Wrap(PQexec(pooled.GetConnection().GetConn(), "ROLLBACK"));
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
      "postgres_guarded_ddl_scoped_v2",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindScopedV2, Init));
  loader.RegisterFunction(TableFunction(
      "postgres_guarded_ddl_scoped_v3",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindScopedV3, Init));
  loader.RegisterFunction(TableFunction(
      "postgres_guarded_ddl_scoped_v4",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindScopedV4, Init));
  loader.RegisterFunction(TableFunction(
      "postgres_guarded_ddl_v2",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
       LogicalType::VARCHAR, LogicalType::ANY},
      Scan, Bind, Init));
  loader.RegisterFunction(TableFunction(
      "postgres_guarded_ddl_v3",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
       LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindV3, Init));
  loader.RegisterFunction(TableFunction(
      "postgres_guarded_ddl_v4",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
       LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindV4, Init));
}
} // namespace duckdb
