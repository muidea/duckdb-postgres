#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_crud_deadline.hpp"
#include "guarded_source_error.hpp"
#include "guarded_ddl_intent.hpp"
#include "postgres_family_metadata.hpp"
#include "storage/postgres_catalog.hpp"

namespace duckdb {
namespace {
struct Data : TableFunctionData {
  PostgresCatalog &catalog;
  GuardedDDLIntent intent;
  string product;
    bool diagnostics = false;
  Data(PostgresCatalog &c, GuardedDDLIntent d, string p)
      : catalog(c), intent(std::move(d)), product(std::move(p)) {}
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
                                             vector<Identifier> &names,
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
  auto cond_it = in.named_parameters.find("conditional");
  const bool conditional = cond_it != in.named_parameters.end() && !cond_it->second.IsNull() &&
                           cond_it->second.GetValue<bool>();
  auto intent = GuardedDDLIntent::Bind(inputs, false, contract, conditional);
  if (scoped) {
    if (intent.action != "drop_index" && intent.action != "rename_index" && intent.action != "reindex")
      throw BinderException("DDL owner scope is unavailable");
    intent.owner = owner;
  }
  const auto &function_name = in.table_function.GetName().GetIdentifierName();
  auto product = function_name.substr(0, function_name.find('_'));
  if (product != POSTGRES_CRUD_PRODUCT && product != GREENPLUM_CRUD_PRODUCT &&
      product != KINGBASE_CRUD_PRODUCT)
    throw BinderException("DDL source product is unavailable");
  auto db = DatabaseManager::Get(ctx).GetDatabase(
      ctx, Identifier(GuardedDDLIntent::Text(in.inputs[0])));
  if (!db || db->GetCatalog().GetCatalogType() != "postgres")
    throw BinderException("DDL catalog is unavailable");
  names = {"operation_id", "status", "sqlstate", "stage", "warning_count"};
  types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
           LogicalType::VARCHAR, LogicalType::BIGINT};
  auto data=make_uniq<Data>(db->GetCatalog().Cast<PostgresCatalog>(), std::move(intent), std::move(product));
  auto diag=in.named_parameters.find("source_diagnostics");
  data->diagnostics=diag!=in.named_parameters.end() && !diag->second.IsNull() && diag->second.GetValue<bool>();
  if(data->diagnostics) {names.push_back("source_error");types.push_back(LogicalType::VARCHAR);}
  return std::move(data);
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx,
                                     TableFunctionBindInput &in,
                                     vector<LogicalType> &types,
                                     vector<Identifier> &names) {
  if (in.inputs.size() != 5 || in.inputs[4].IsNull() ||
      in.inputs[4].type().id() != LogicalTypeId::STRUCT) {
    throw BinderException("DDL structured input is invalid");
  }
  const auto &st = StructType::GetChildTypes(in.inputs[4].type());
  const auto &sf = StructValue::GetChildren(in.inputs[4]);
  if (st.empty() || st[0].first != "contract_version" || sf[0].IsNull() ||
      sf[0].type().id() != LogicalTypeId::INTEGER) {
    throw BinderException("DDL contract is unavailable");
  }
  const int32_t contract = sf[0].GetValue<int32_t>();
  if (contract < 2 || contract > 6) {
    throw BinderException("DDL contract is unavailable");
  }
  const bool has_owner = st.size() >= 6 && st[5].first == "owner" &&
                         !sf[5].IsNull() &&
                         sf[5].type().id() == LogicalTypeId::VARCHAR &&
                         !sf[5].GetValue<string>().empty();
  return BindContract(ctx, in, types, names, contract, has_owner);
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
static GuardedSourceError SourceError(PGresult *result, const string &product) {
    GuardedSourceError e;
    if (!result || !PQresultErrorField(result, PG_DIAG_SQLSTATE)) return e;
    auto field = [&](int code) { auto value=PQresultErrorField(result, code); return value ? string(value) : string(); };
    e.product=product;e.state=field(PG_DIAG_SQLSTATE);e.code=e.state;e.message=field(PG_DIAG_MESSAGE_PRIMARY);
    e.detail=field(PG_DIAG_MESSAGE_DETAIL);e.hint=field(PG_DIAG_MESSAGE_HINT);e.schema=field(PG_DIAG_SCHEMA_NAME);
    e.table=field(PG_DIAG_TABLE_NAME);e.column=field(PG_DIAG_COLUMN_NAME);e.constraint=field(PG_DIAG_CONSTRAINT_NAME);
    return e;
}
static void Scan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
  auto &state = in.global_state->Cast<State>();
  if (state.done)
    return;
  state.done = true;
  auto &d = in.bind_data->Cast<Data>();
  string status = "CONFIRMED_NOT_EXECUTED", error, stage = "source_identity";
  GuardedSourceError source;
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
    if (!OK(reset.get())) {
      source = SourceError(reset.get(), d.product); error = ErrorState(reset.get());
      throw IOException("DDL reset failed");
    }
    auto identity = Wrap(PQexec(
        raw,
        "SELECT "
        "current_database(),version(),current_setting('server_version_num')"));
    if (!identity || PQresultStatus(identity.get()) != PGRES_TUPLES_OK ||
        PQntuples(identity.get()) != 1) {
      source = SourceError(identity.get(), d.product);
      throw IOException("DDL source identity unavailable");
    }
    string version = PQgetvalue(identity.get(), 0, 1);
    string server_num_str = PQgetvalue(identity.get(), 0, 2);
    int64_t server_num = 0;
    try {
      server_num = std::stoll(server_num_str);
    } catch (...) {
      server_num = 0;
    }
    bool is_greenplum = version.find("Greenplum") != string::npos;
    bool is_kingbase = version.find("Kingbase") != string::npos;
    if (!is_kingbase) {
      auto mode_probe = Wrap(PQexec(raw, "SELECT 1 FROM pg_catalog.pg_settings WHERE name='database_mode'"));
      if (mode_probe && PQresultStatus(mode_probe.get()) == PGRES_TUPLES_OK && PQntuples(mode_probe.get()) == 1)
        is_kingbase = true;
    }
    const bool identity_ok =
        string(PQgetvalue(identity.get(), 0, 0)) == d.intent.database &&
        (d.product == GREENPLUM_CRUD_PRODUCT
             ? is_greenplum
             : (d.product == KINGBASE_CRUD_PRODUCT
                    ? is_kingbase
                    : (version.find("PostgreSQL ") == 0 && !is_greenplum && !is_kingbase)));
    if (!identity_ok) {
      error = "0A000";
      throw IOException("DDL source not certified");
    }
    const string not_partition = server_num >= 100000 ? "AND NOT t.relispartition " : "";
    const string not_c_partition = server_num >= 100000 ? "AND NOT c.relispartition " : "";
    const bool schema_action = d.intent.action == "create_schema" || d.intent.action == "drop_schema";
    auto begin = Wrap(PQexec(raw, d.intent.owner.empty() ? "BEGIN" : "BEGIN ISOLATION LEVEL READ COMMITTED"));
    if (!OK(begin.get())) {
      source = SourceError(begin.get(), d.product); error = ErrorState(begin.get());
      throw IOException("DDL begin failed");
    }
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
      source=SourceError(settings.get(), d.product); error = ErrorState(settings.get());
      throw IOException("DDL settings failed");
    }
    if (ctx.IsInterrupted())
      throw InterruptException();
    string scoped_index_oid;
    bool skip_absent_index = false;
    if (!d.intent.owner.empty()) {
      // The persisted application scope is authority only after acquiring the
      // expected table's lock and checking index ownership on this exact source
      // transaction. A renamed/replaced index cannot escape that table scope.
      const auto target = KeywordHelper::WriteQuoted(d.intent.schema, '"') + "." +
                          KeywordHelper::WriteQuoted(d.intent.owner, '"');
      auto lock = Wrap(PQexec(raw, ("LOCK TABLE ONLY " + target + " IN ACCESS EXCLUSIVE MODE").c_str()));
      if (!OK(lock.get())) {
        source=SourceError(lock.get(), d.product); error = ErrorState(lock.get());
        throw IOException("DDL owner lock unavailable");
      }
      const char *values[] = {d.intent.schema.c_str(), d.intent.name.c_str(), d.intent.owner.c_str()};
      auto owner = Wrap(PQexecParams(raw,
          (string("SELECT i.oid::text FROM pg_catalog.pg_class i ") +
          "JOIN pg_catalog.pg_namespace n ON n.oid=i.relnamespace "
          "JOIN pg_catalog.pg_index x ON x.indexrelid=i.oid "
          "JOIN pg_catalog.pg_class t ON t.oid=x.indrelid "
          "WHERE n.nspname=$1 AND i.relname=$2 AND i.relkind='i' "
          "AND t.relnamespace=n.oid AND t.relname=$3 AND t.relkind='r' "
          "AND t.relpersistence='p' " + not_partition +
          "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_inherits h "
          "WHERE h.inhrelid=t.oid OR h.inhparent=t.oid)").c_str(),
          3, nullptr, values, nullptr, nullptr, 0));
      if (!owner || PQresultStatus(owner.get()) != PGRES_TUPLES_OK) {
        error = "0A000";
        throw IOException("DDL index owner check failed");
      }
      if (PQntuples(owner.get()) != 1) {
        if (d.intent.conditional && d.intent.action == "drop_index" && PQntuples(owner.get()) == 0) {
          auto any_same = Wrap(PQexecParams(raw,
              "SELECT 1 FROM pg_catalog.pg_class i JOIN pg_catalog.pg_namespace n ON n.oid=i.relnamespace "
              "WHERE n.nspname=$1 AND i.relname=$2 AND i.relkind='i'",
              2, nullptr, values, nullptr, nullptr, 0));
          if (!any_same || PQresultStatus(any_same.get()) != PGRES_TUPLES_OK || PQntuples(any_same.get()) != 0) {
            error = "0A000";
            throw IOException("DDL index owner changed");
          }
          skip_absent_index = true;
        } else {
          error = "0A000";
          throw IOException("DDL index owner changed");
        }
      } else {
        scoped_index_oid = PQgetvalue(owner.get(), 0, 0);
      }
    } else if (d.intent.action == "drop_index" && d.intent.conditional) {
      const char *values[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
      auto any_same = Wrap(PQexecParams(raw,
          "SELECT 1 FROM pg_catalog.pg_class i JOIN pg_catalog.pg_namespace n ON n.oid=i.relnamespace "
          "WHERE n.nspname=$1 AND i.relname=$2 AND i.relkind='i'",
          2, nullptr, values, nullptr, nullptr, 0));
      if (!any_same || PQresultStatus(any_same.get()) != PGRES_TUPLES_OK || PQntuples(any_same.get()) != 0) {
        error = "0A000";
        throw IOException("DDL index owner scope required");
      }
      skip_absent_index = true;
    }
    if (d.intent.contract_version >= 3 && !schema_action) {
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
                        "' AND c.relpersistence='p' " +
                        not_c_partition +
                        "AND NOT EXISTS (SELECT 1 "
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
    if (!skip_absent_index) {
      stage = "source_execute";
      sent = true;
      auto result = Wrap(PQexec(raw, d.intent.sql.c_str()));
      if (!OK(result.get())) {
        source=SourceError(result.get(), d.product); error = ErrorState(result.get());
        throw IOException("DDL execution failed");
      }
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
      source=SourceError(commit.get(), d.product); error = ErrorState(commit.get());
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
  if(d.diagnostics) out.SetValue(5,0,Value(source.JSON()));
  out.SetValue(0, 0, Value(d.intent.operation));
  out.SetValue(1, 0, Value(status));
  out.SetValue(2, 0, Value(error));
  out.SetValue(3, 0, Value(stage));
  out.SetValue(4, 0, Value::BIGINT(0));
  out.SetChildCardinality(1);
}
} // namespace
void RegisterPostgresGuardedDDL(ExtensionLoader &loader) {
  auto register_function = [&](TableFunction function) {
    function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
      options.Add("source_diagnostics", LogicalType::BOOLEAN);
      options.Add("conditional", LogicalType::BOOLEAN);
    });
    loader.RegisterFunction(function);
  };
  for (auto &product :
       {POSTGRES_CRUD_PRODUCT, GREENPLUM_CRUD_PRODUCT, KINGBASE_CRUD_PRODUCT}) {
    register_function(TableFunction(Identifier(string(product) + "_guarded_ddl"),
                                    {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                     LogicalType::VARCHAR, LogicalType::VARCHAR,
                                     LogicalType::ANY},
                                    Scan, Bind, Init));
  }
}
} // namespace duckdb
