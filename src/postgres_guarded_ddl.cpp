#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_crud_deadline.hpp"
#include "guarded_source_error.hpp"
#include "guarded_ddl_intent.hpp"
#include "storage/postgres_catalog.hpp"

namespace duckdb {
namespace {
struct Data : TableFunctionData {
  PostgresCatalog &catalog;
  GuardedDDLIntent intent;
  string product = "postgres";
  bool unified_v1 = false;
  bool diagnostics = false;
  Data(PostgresCatalog &c, GuardedDDLIntent d)
      : catalog(c), intent(std::move(d)), product(intent.product),
        unified_v1(intent.unified_v1) {}
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
                                             int32_t contract, bool scoped = false,
                                             const string &product = "postgres",
                                             bool unified_v1 = false) {
  auto inputs = in.inputs;
  string owner;
  if (scoped) {
    if (inputs.size() != 5 || inputs[4].IsNull() || inputs[4].type().id() != LogicalTypeId::STRUCT)
      throw BinderException("DDL owner scope is unavailable");
    const auto &ctypes = StructType::GetChildTypes(inputs[4].type());
    const auto &fields = StructValue::GetChildren(inputs[4]);
    if (ctypes.size() < 6 || ctypes[5].first != "owner")
      throw BinderException("DDL owner scope is unavailable");
    owner = GuardedDDLIntent::Text(fields[5]);
    if (!GuardedDDLIntent::Name(owner))
      throw BinderException("DDL owner scope is unavailable");
    child_list_t<Value> values;
    for (idx_t i = 0; i < ctypes.size(); i++)
      values.emplace_back(ctypes[i].first, i == 5 ? Value("") : fields[i]);
    inputs[4] = Value::STRUCT(std::move(values));
  }
  auto intent = (contract == 6 || unified_v1)
                    ? GuardedDDLIntent::BindV6(inputs, contract, product, unified_v1)
                    : GuardedDDLIntent::Bind(inputs, false, contract);
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
  auto data=make_uniq<Data>(db->GetCatalog().Cast<PostgresCatalog>(), std::move(intent));
  auto diag=in.named_parameters.find("source_diagnostics");
  data->diagnostics=diag!=in.named_parameters.end() && !diag->second.IsNull() && diag->second.GetValue<bool>();
  if(data->diagnostics) {names.push_back("source_error");types.push_back(LogicalType::VARCHAR);}
  return std::move(data);
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
static unique_ptr<FunctionData> BindV5(ClientContext &ctx,
                                       TableFunctionBindInput &in,
                                       vector<LogicalType> &types,
                                       vector<string> &names) {
  return BindContract(ctx, in, types, names, 5);
}
static unique_ptr<FunctionData> BindV6(ClientContext &ctx,
                                       TableFunctionBindInput &in,
                                       vector<LogicalType> &types,
                                       vector<string> &names) {
  return BindContract(ctx, in, types, names, 6);
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
static unique_ptr<FunctionData> BindScopedV6(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
  return BindContract(ctx, in, types, names, 6, true);
}
static unique_ptr<FunctionData> BindGreenplumV1(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
  return BindContract(ctx, in, types, names, 1, false, "greenplum", true);
}
static unique_ptr<FunctionData> BindKingbaseV1(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
  return BindContract(ctx, in, types, names, 1, false, "kingbasees", true);
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
    e.product=product.empty() ? string("postgres") : product;e.state=field(PG_DIAG_SQLSTATE);e.code=e.state;e.message=field(PG_DIAG_MESSAGE_PRIMARY);
    e.detail=field(PG_DIAG_MESSAGE_DETAIL);e.hint=field(PG_DIAG_MESSAGE_HINT);e.schema=field(PG_DIAG_SCHEMA_NAME);
    e.table=field(PG_DIAG_TABLE_NAME);e.column=field(PG_DIAG_COLUMN_NAME);e.constraint=field(PG_DIAG_CONSTRAINT_NAME);
    return e;
}
static bool ProductIdentityMatches(const string &product, const string &version) {
  if (product == "postgres") {
    return version.find("PostgreSQL ") == 0 && version.find("Greenplum") == string::npos;
  }
  if (product == "greenplum") {
    return version.find("PostgreSQL ") == 0 && version.find("(Greenplum Database ") != string::npos;
  }
  if (product == "kingbasees") {
    return version.find("KingbaseES ") == 0;
  }
  return false;
}
static unsigned int DDLTimeoutSeconds(const GuardedDDLIntent &intent) {
  if (intent.action == "reindex") {
    return 300;
  }
  if (intent.contract_version == 4 || intent.action == "alter_column_type" ||
      intent.action == "set_default" || intent.action == "drop_default" ||
      intent.action == "set_not_null" || intent.action == "drop_not_null" ||
      intent.action == "add_constraint" || intent.action == "drop_constraint" ||
      intent.action == "truncate") {
    return 120;
  }
  return 30;
}
static bool PGFlag(PGresult *r, int row, int col) {
  if (!r || PQgetisnull(r, row, col)) {
    return false;
  }
  const char *v = PQgetvalue(r, row, col);
  return v && (*v == 't' || *v == '1' || strcmp(v, "true") == 0);
}
static vector<int32_t> ParseInt2Vector(const string &text) {
  vector<int32_t> out;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
      i++;
    }
    if (i >= text.size()) {
      break;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') {
      j++;
    }
    out.push_back(static_cast<int32_t>(std::stoi(text.substr(i, j - i))));
    i = j;
  }
  return out;
}
static bool IsLosslessTypeWidening(uint32_t typid, int32_t typmod, const DDLColumnSpec &target,
                                   const string &product) {
  const auto &to = target.type;
  switch (typid) {
  case 21: // int2 / smallint
    return to == "integer" || to == "bigint";
  case 23: // int4 / integer
    return to == "bigint";
  case 700: // float4 / real
    return to == "double";
  case 1700: { // numeric(p,s)
    if (to != "numeric" || typmod < 4) {
      return false;
    }
    int32_t packed = typmod - 4;
    int32_t cur_p = (packed >> 16) & 0xffff;
    int32_t cur_s = packed & 0xffff;
    return target.scale == cur_s && target.precision > cur_p && target.precision <= 38;
  }
  case 1043: { // varchar(n)
    if (typmod < 4) {
      return false;
    }
    int32_t cur_len = typmod - 4;
    if (to == "varchar") {
      return target.length > cur_len && target.length <= 10485760;
    }
    return to == "text" && (product == "postgres" || product == "greenplum" || product == "kingbasees");
  }
  case 1083: // time without time zone
  case 1114: // timestamp without time zone
  case 1184: { // timestamptz
    string cur_name = typid == 1083 ? "time" : (typid == 1114 ? "timestamp" : "timestamptz");
    if (to != cur_name) {
      return false;
    }
    int32_t cur_prec = typmod == -1 ? 6 : typmod;
    return target.precision > cur_prec && target.precision <= 6;
  }
  default:
    return false;
  }
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
    const unsigned int timeout_secs = DDLTimeoutSeconds(d.intent);
    deadline = make_uniq<GuardedCRUDDeadline>(ctx, PQsocket(raw), timeout_secs);
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
    if (string(PQgetvalue(identity.get(), 0, 0)) != d.intent.database ||
        !ProductIdentityMatches(d.product, version)) {
      error = "0A000";
      throw IOException("DDL source not certified");
    }
    const bool use_read_committed = !d.intent.owner.empty() || d.unified_v1 || d.intent.contract_version == 6;
    auto begin = Wrap(PQexec(raw, use_read_committed ? "BEGIN ISOLATION LEVEL READ COMMITTED" : "BEGIN"));
    if (!OK(begin.get())) {
      source = SourceError(begin.get(), d.product); error = ErrorState(begin.get());
      throw IOException("DDL begin failed");
    }
    transaction = true;
    const string timeout = timeout_secs == 300 ? "290s" : (timeout_secs == 120 ? "110s" : "25s");
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

    auto catalog_probe = Wrap(PQexec(raw,
        "SELECT "
        "EXISTS(SELECT 1 FROM pg_catalog.pg_attribute WHERE attrelid='pg_catalog.pg_class'::regclass AND attname='relispartition' AND NOT attisdropped),"
        "EXISTS(SELECT 1 FROM pg_catalog.pg_attribute WHERE attrelid='pg_catalog.pg_class'::regclass AND attname='relstorage' AND NOT attisdropped),"
        "EXISTS(SELECT 1 FROM pg_catalog.pg_attribute WHERE attrelid='pg_catalog.pg_attribute'::regclass AND attname='attidentity' AND NOT attisdropped),"
        "EXISTS(SELECT 1 FROM pg_catalog.pg_attribute WHERE attrelid='pg_catalog.pg_attribute'::regclass AND attname='attgenerated' AND NOT attisdropped)"));
    if (!catalog_probe || PQresultStatus(catalog_probe.get()) != PGRES_TUPLES_OK || PQntuples(catalog_probe.get()) != 1) {
      source = SourceError(catalog_probe.get(), d.product);
      error = ErrorState(catalog_probe.get());
      throw IOException("DDL catalog probe failed");
    }
    const bool has_relispartition = PGFlag(catalog_probe.get(), 0, 0);
    const bool has_relstorage = PGFlag(catalog_probe.get(), 0, 1);
    const bool has_attidentity = PGFlag(catalog_probe.get(), 0, 2);
    const bool has_attgenerated = PGFlag(catalog_probe.get(), 0, 3);
    const string c_part_cond = has_relispartition ? " AND NOT c.relispartition" : "";
    const string t_part_cond = has_relispartition ? " AND NOT t.relispartition" : "";
    const string c_stor_cond = (d.product == "greenplum" && has_relstorage) ? " AND c.relstorage='h'" : "";
    const string t_stor_cond = (d.product == "greenplum" && has_relstorage) ? " AND t.relstorage='h'" : "";
    const string attid_expr = has_attidentity ? "a.attidentity::text" : "''::text";
    const string attgen_expr = has_attgenerated ? "a.attgenerated::text" : "''::text";
    const bool extended = (d.intent.contract_version == 6 || d.unified_v1);
    bool noop = false;

    stage = "source_definition";
    if (d.product == "kingbasees") {
      auto kb_mode = Wrap(PQexec(raw, "SELECT current_setting('database_mode'), coalesce(current_setting('ora_input_emptystr_isnull', true), '')"));
      if (!kb_mode || PQresultStatus(kb_mode.get()) != PGRES_TUPLES_OK || PQntuples(kb_mode.get()) != 1) {
        source = SourceError(kb_mode.get(), d.product);
        error = ErrorState(kb_mode.get());
        throw IOException("DDL KingbaseES mode probe failed");
      }
      const string mode = PQgetvalue(kb_mode.get(), 0, 0);
      const string empty_null = PQgetvalue(kb_mode.get(), 0, 1);
      if (mode == "oracle" || empty_null == "on") {
        if (d.intent.action == "set_default" && d.intent.default_kind == "string" && d.intent.default_value.empty()) {
          error = "0A000";
          throw IOException("DDL empty string default is NULL in oracle mode");
        }
        for (const auto &spec : d.intent.column_specs) {
          if (spec.default_kind == "string" && spec.default_value.empty()) {
            error = "0A000";
            throw IOException("DDL empty string default is NULL in oracle mode");
          }
        }
      }
    }
    if (d.product == "greenplum" && d.intent.action == "add_constraint" && d.intent.constraint_kind == "foreign_key") {
      error = "0A000";
      throw IOException("Greenplum does not enforce foreign key constraints");
    }

    auto finish_noop = [&]() {
      auto rollback = Wrap(PQexec(raw, "ROLLBACK"));
      if (!OK(rollback.get())) {
        source = SourceError(rollback.get(), d.product);
        error = ErrorState(rollback.get());
        throw IOException("DDL noop cleanup failed");
      }
      transaction = false;
      status = "COMMITTED";
      stage = "source_noop";
      noop = true;
    };

    if (extended && d.intent.conditional && d.intent.action != "drop_index") {
      if (d.intent.action == "create_table") {
        const char *values[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
        auto exists = Wrap(PQexecParams(raw,
            ("SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
             "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='r' AND c.relpersistence='p'" +
             c_part_cond + c_stor_cond +
             " AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_inherits h WHERE h.inhrelid=c.oid OR h.inhparent=c.oid)")
                .c_str(),
            2, nullptr, values, nullptr, nullptr, 0));
        if (exists && PQresultStatus(exists.get()) == PGRES_TUPLES_OK && PQntuples(exists.get()) == 1) {
          finish_noop();
        }
      } else if (d.intent.action == "drop_table") {
        const char *values[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
        auto present = Wrap(PQexecParams(raw,
            "SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=$1 AND c.relname=$2",
            2, nullptr, values, nullptr, nullptr, 0));
        if (!present || PQresultStatus(present.get()) != PGRES_TUPLES_OK) {
          source = SourceError(present.get(), d.product);
          error = ErrorState(present.get());
          throw IOException("DDL table presence check failed");
        }
        if (PQntuples(present.get()) == 0) {
          finish_noop();
        }
      } else if (d.intent.action == "add_column" || d.intent.action == "drop_column") {
        const string &col_name = d.intent.action == "add_column" ? d.intent.column_specs[0].name : d.intent.column_name;
        const char *tvalues[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
        auto tbl = Wrap(PQexecParams(raw,
            ("SELECT c.oid::text FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
             "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='r' AND c.relpersistence='p'" +
             c_part_cond + c_stor_cond +
             " AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_inherits h WHERE h.inhrelid=c.oid OR h.inhparent=c.oid)")
                .c_str(),
            2, nullptr, tvalues, nullptr, nullptr, 0));
        if (tbl && PQresultStatus(tbl.get()) == PGRES_TUPLES_OK && PQntuples(tbl.get()) == 1) {
          string oid = PQgetvalue(tbl.get(), 0, 0);
          const char *cvalues[] = {oid.c_str(), col_name.c_str()};
          auto col = Wrap(PQexecParams(raw,
              "SELECT 1 FROM pg_catalog.pg_attribute WHERE attrelid=$1::oid AND attname=$2 AND attnum>0 AND NOT attisdropped",
              2, nullptr, cvalues, nullptr, nullptr, 0));
          if (!col || PQresultStatus(col.get()) != PGRES_TUPLES_OK) {
            source = SourceError(col.get(), d.product);
            error = ErrorState(col.get());
            throw IOException("DDL column presence check failed");
          }
          if ((d.intent.action == "add_column" && PQntuples(col.get()) == 1) ||
              (d.intent.action == "drop_column" && PQntuples(col.get()) == 0)) {
            finish_noop();
          }
        }
      } else if (d.intent.action == "create_index" || d.intent.action == "create_unique_index") {
        const char *tvalues[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
        auto tbl = Wrap(PQexecParams(raw,
            ("SELECT c.oid::text FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
             "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='r' AND c.relpersistence='p'" +
             c_part_cond + c_stor_cond +
             " AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_inherits h WHERE h.inhrelid=c.oid OR h.inhparent=c.oid)")
                .c_str(),
            2, nullptr, tvalues, nullptr, nullptr, 0));
        if (tbl && PQresultStatus(tbl.get()) == PGRES_TUPLES_OK && PQntuples(tbl.get()) == 1) {
          string toid = PQgetvalue(tbl.get(), 0, 0);
          const char *ivalues[] = {d.intent.schema.c_str(), d.intent.index.c_str(), toid.c_str()};
          auto idx = Wrap(PQexecParams(raw,
              "SELECT 1 FROM pg_catalog.pg_class i JOIN pg_catalog.pg_namespace n ON n.oid=i.relnamespace "
              "JOIN pg_catalog.pg_index x ON x.indexrelid=i.oid "
              "WHERE n.nspname=$1 AND i.relname=$2 AND i.relkind='i' AND x.indrelid=$3::oid",
              3, nullptr, ivalues, nullptr, nullptr, 0));
          if (idx && PQresultStatus(idx.get()) == PGRES_TUPLES_OK && PQntuples(idx.get()) == 1) {
            finish_noop();
          }
        }
      }
    }

    string scoped_index_oid;
    if (!noop && !d.intent.owner.empty()) {
      // The persisted application scope is authority only after acquiring the
      // expected table's lock and checking index ownership on this exact source
      // transaction. A renamed/replaced index cannot escape that table scope.
      stage = "source_lock";
      const auto target = KeywordHelper::WriteQuoted(d.intent.schema, '"') + "." +
                          KeywordHelper::WriteQuoted(d.intent.owner, '"');
      auto lock = Wrap(PQexec(raw, ("LOCK TABLE ONLY " + target + " IN ACCESS EXCLUSIVE MODE").c_str()));
      if (!OK(lock.get())) {
        source=SourceError(lock.get(), d.product); error = ErrorState(lock.get());
        throw IOException("DDL owner lock unavailable");
      }
      stage = "source_definition";
      const char *values[] = {d.intent.schema.c_str(), d.intent.name.c_str(), d.intent.owner.c_str()};
      auto owner = Wrap(PQexecParams(raw,
          ("SELECT i.oid::text FROM pg_catalog.pg_class i "
           "JOIN pg_catalog.pg_namespace n ON n.oid=i.relnamespace "
           "JOIN pg_catalog.pg_index x ON x.indexrelid=i.oid "
           "JOIN pg_catalog.pg_class t ON t.oid=x.indrelid "
           "WHERE n.nspname=$1 AND i.relname=$2 AND i.relkind='i' "
           "AND t.relnamespace=n.oid AND t.relname=$3 AND t.relkind='r' "
           "AND t.relpersistence='p'" + t_part_cond + t_stor_cond +
           " AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_inherits h "
           "WHERE h.inhrelid=t.oid OR h.inhparent=t.oid)").c_str(),
          3, nullptr, values, nullptr, nullptr, 0));
      if (!owner || PQresultStatus(owner.get()) != PGRES_TUPLES_OK) {
        source = SourceError(owner.get(), d.product);
        error = ErrorState(owner.get());
        throw IOException("DDL index owner query failed");
      }
      if (PQntuples(owner.get()) != 1) {
        if (extended && d.intent.action == "drop_index" && d.intent.conditional) {
          const char *pvalues[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
          auto any_obj = Wrap(PQexecParams(raw,
              "SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=$1 AND c.relname=$2",
              2, nullptr, pvalues, nullptr, nullptr, 0));
          if (any_obj && PQresultStatus(any_obj.get()) == PGRES_TUPLES_OK && PQntuples(any_obj.get()) == 0) {
            finish_noop();
          }
        }
        if (!noop) {
          error = "0A000";
          throw IOException("DDL index owner changed");
        }
      } else {
        scoped_index_oid = PQgetvalue(owner.get(), 0, 0);
      }
    } else if (!noop && extended && d.intent.action == "drop_index" && d.intent.conditional) {
      const char *pvalues[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
      auto any_obj = Wrap(PQexecParams(raw,
          "SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=$1 AND c.relname=$2",
          2, nullptr, pvalues, nullptr, nullptr, 0));
      if (!any_obj || PQresultStatus(any_obj.get()) != PGRES_TUPLES_OK) {
        source = SourceError(any_obj.get(), d.product);
        error = ErrorState(any_obj.get());
        throw IOException("DDL index presence check failed");
      }
      if (PQntuples(any_obj.get()) == 0) {
        finish_noop();
      }
    }

    if (!noop && (d.intent.contract_version >= 3 || d.unified_v1)) {
      // Read-only scope check; no extra source object lock. EXECUTION_NAME
      // still applies to the subsequent native utility statement.
      const char *values[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
      const string kind =
          (d.intent.action == "drop_index" || d.intent.action == "rename_index" || d.intent.action == "reindex")
              ? "i"
              : "r";
      const string rel_filter = (kind == "r") ? (c_part_cond + c_stor_cond) : c_part_cond;
      auto shape = Wrap(
          PQexecParams(raw,
                       ("SELECT 1 FROM pg_catalog.pg_class c JOIN "
                        "pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
                        "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='" +
                        kind +
                        "' AND c.relpersistence='p'" + rel_filter +
                        " AND NOT EXISTS (SELECT 1 "
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

    if (!noop && extended && d.intent.action == "add_column" && d.intent.column_specs.size() == 1 &&
        d.intent.column_specs[0].not_null && d.intent.column_specs[0].default_kind.empty()) {
      const char *tvalues[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
      auto tbl = Wrap(PQexecParams(raw,
          ("SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
           "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='r' AND c.relpersistence='p'" +
           c_part_cond + c_stor_cond).c_str(),
          2, nullptr, tvalues, nullptr, nullptr, 0));
      if (tbl && PQresultStatus(tbl.get()) == PGRES_TUPLES_OK && PQntuples(tbl.get()) == 1) {
        const auto target = KeywordHelper::WriteQuoted(d.intent.schema, '"') + "." +
                            KeywordHelper::WriteQuoted(d.intent.name, '"');
        auto non_empty = Wrap(PQexec(raw, ("SELECT EXISTS (SELECT 1 FROM " + target + " LIMIT 1)").c_str()));
        if (!non_empty || PQresultStatus(non_empty.get()) != PGRES_TUPLES_OK || PQntuples(non_empty.get()) != 1) {
          source = SourceError(non_empty.get(), d.product);
          error = ErrorState(non_empty.get());
          throw IOException("DDL add_column non-empty check failed");
        }
        if (PGFlag(non_empty.get(), 0, 0)) {
          error = "23502";
          throw IOException("DDL cannot add NOT NULL column without DEFAULT to non-empty table");
        }
      }
    }

    if (!noop && d.product == "greenplum" && d.intent.action == "add_constraint" &&
        (d.intent.constraint_kind == "primary_key" || d.intent.constraint_kind == "unique")) {
      const char *tvalues[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
      auto pol = Wrap(PQexecParams(raw,
          "SELECT c.oid::text, p.policytype::text, coalesce(p.distkey::text,'') "
          "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
          "LEFT JOIN pg_catalog.gp_distribution_policy p ON p.localoid=c.oid "
          "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='r'",
          2, nullptr, tvalues, nullptr, nullptr, 0));
      if (pol && PQresultStatus(pol.get()) == PGRES_TUPLES_OK && PQntuples(pol.get()) == 1) {
        string toid = PQgetvalue(pol.get(), 0, 0);
        string ptype = PQgetvalue(pol.get(), 0, 1);
        vector<int32_t> dist_attnums = ParseInt2Vector(PQgetvalue(pol.get(), 0, 2));
        if (ptype != "p" || dist_attnums.empty()) {
          error = "0A000";
          throw IOException("Greenplum primary/unique constraint requires hash distribution key");
        }
        for (int32_t attnum : dist_attnums) {
          string attnum_str = std::to_string(attnum);
          const char *avalues[] = {toid.c_str(), attnum_str.c_str()};
          auto att = Wrap(PQexecParams(raw,
              "SELECT attname FROM pg_catalog.pg_attribute WHERE attrelid=$1::oid AND attnum=$2::int2 AND NOT attisdropped",
              2, nullptr, avalues, nullptr, nullptr, 0));
          if (!att || PQresultStatus(att.get()) != PGRES_TUPLES_OK || PQntuples(att.get()) != 1) {
            error = "0A000";
            throw IOException("Greenplum distribution key attribute missing");
          }
          string colname = PQgetvalue(att.get(), 0, 0);
          if (std::find(d.intent.constraint_keys.begin(), d.intent.constraint_keys.end(), colname) ==
              d.intent.constraint_keys.end()) {
            error = "0A000";
            throw IOException("Greenplum constraint must contain all distribution key columns");
          }
        }
      }
    }

    if (!noop && extended && (d.intent.action == "drop_column" || d.intent.action == "alter_column_type")) {
      const char *tvalues[] = {d.intent.schema.c_str(), d.intent.name.c_str()};
      auto tbl_pre = Wrap(PQexecParams(raw,
          ("SELECT c.oid::text FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
           "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='r' AND c.relpersistence='p'" +
           c_part_cond + c_stor_cond).c_str(),
          2, nullptr, tvalues, nullptr, nullptr, 0));
      if (tbl_pre && PQresultStatus(tbl_pre.get()) == PGRES_TUPLES_OK && PQntuples(tbl_pre.get()) == 1) {
        stage = "source_lock";
        const auto target = KeywordHelper::WriteQuoted(d.intent.schema, '"') + "." +
                            KeywordHelper::WriteQuoted(d.intent.name, '"');
        auto lock = Wrap(PQexec(raw, ("LOCK TABLE ONLY " + target + " IN ACCESS EXCLUSIVE MODE").c_str()));
        if (!OK(lock.get())) {
          source = SourceError(lock.get(), d.product);
          error = ErrorState(lock.get());
          throw IOException("DDL column lock unavailable");
        }
        stage = "source_definition";
        auto tbl = Wrap(PQexecParams(raw,
            ("SELECT c.oid::text FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
             "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind='r' AND c.relpersistence='p'" +
             c_part_cond + c_stor_cond).c_str(),
            2, nullptr, tvalues, nullptr, nullptr, 0));
        if (tbl && PQresultStatus(tbl.get()) == PGRES_TUPLES_OK && PQntuples(tbl.get()) == 1) {
          string toid = PQgetvalue(tbl.get(), 0, 0);
          const char *cvalues[] = {toid.c_str(), d.intent.column_name.c_str()};
          auto col = Wrap(PQexecParams(raw,
              ("SELECT a.attnum::int4, a.atttypid::int8, a.atttypmod, " + attid_expr + ", " + attgen_expr +
               " FROM pg_catalog.pg_attribute a WHERE a.attrelid=$1::oid AND a.attname=$2 AND a.attnum>0 AND NOT a.attisdropped")
                  .c_str(),
              2, nullptr, cvalues, nullptr, nullptr, 0));
          if (!col || PQresultStatus(col.get()) != PGRES_TUPLES_OK) {
            source = SourceError(col.get(), d.product);
            error = ErrorState(col.get());
            throw IOException("DDL column inspection failed");
          }
          if (PQntuples(col.get()) == 0) {
            if (d.intent.action == "drop_column" && d.intent.conditional) {
              finish_noop();
            }
          } else {
            string attnum_str = PQgetvalue(col.get(), 0, 0);
            int32_t attnum = std::stoi(attnum_str);
            uint32_t typid = static_cast<uint32_t>(std::stoul(PQgetvalue(col.get(), 0, 1)));
            int32_t typmod = std::stoi(PQgetvalue(col.get(), 0, 2));
            string attidentity = PQgetvalue(col.get(), 0, 3);
            string attgenerated = PQgetvalue(col.get(), 0, 4);

            if (d.intent.action == "drop_column") {
              const char *cnt_vals[] = {toid.c_str()};
              auto cnt = Wrap(PQexecParams(raw,
                  "SELECT count(*)::int8 FROM pg_catalog.pg_attribute WHERE attrelid=$1::oid AND attnum>0 AND NOT attisdropped",
                  1, nullptr, cnt_vals, nullptr, nullptr, 0));
              if (!cnt || PQresultStatus(cnt.get()) != PGRES_TUPLES_OK || PQntuples(cnt.get()) != 1) {
                source = SourceError(cnt.get(), d.product);
                error = ErrorState(cnt.get());
                throw IOException("DDL column count failed");
              }
              if (std::stoll(PQgetvalue(cnt.get(), 0, 0)) <= 1) {
                error = "42P16";
                throw IOException("DDL cannot drop the only remaining column");
              }
            }

            if (!attidentity.empty() || !attgenerated.empty()) {
              error = "2BP01";
              throw IOException("DDL cannot modify or drop identity/generated column");
            }
            const char *dep_vals[] = {toid.c_str(), attnum_str.c_str()};
            auto dep = Wrap(PQexecParams(raw,
                "SELECT "
                "EXISTS(SELECT 1 FROM pg_catalog.pg_index i WHERE i.indrelid=$1::oid AND ($2::int2 = ANY(i.indkey::int2[]))),"
                "EXISTS(SELECT 1 FROM pg_catalog.pg_constraint k WHERE (k.conrelid=$1::oid AND $2::int2 = ANY(k.conkey)) OR (k.confrelid=$1::oid AND $2::int2 = ANY(k.confkey))),"
                "EXISTS(SELECT 1 FROM pg_catalog.pg_depend dep WHERE dep.refclassid='pg_catalog.pg_class'::regclass AND dep.refobjid=$1::oid AND dep.refobjsubid=$2::int4 AND dep.deptype IN ('n','a','i') AND NOT (dep.classid='pg_catalog.pg_attrdef'::regclass AND EXISTS(SELECT 1 FROM pg_catalog.pg_attrdef ad WHERE ad.oid=dep.objid AND ad.adrelid=$1::oid AND ad.adnum=$2::int2)))",
                2, nullptr, dep_vals, nullptr, nullptr, 0));
            if (!dep || PQresultStatus(dep.get()) != PGRES_TUPLES_OK || PQntuples(dep.get()) != 1) {
              source = SourceError(dep.get(), d.product);
              error = ErrorState(dep.get());
              throw IOException("DDL dependency check failed");
            }
            if (PGFlag(dep.get(), 0, 0) || PGFlag(dep.get(), 0, 1) || PGFlag(dep.get(), 0, 2)) {
              error = "2BP01";
              throw IOException("DDL column has dependent indexes, constraints, sequences, views, or expressions");
            }
            if (d.product == "greenplum") {
              const char *dist_vals[] = {toid.c_str()};
              auto dist = Wrap(PQexecParams(raw,
                  "SELECT coalesce(p.distkey::text,'') FROM pg_catalog.gp_distribution_policy p WHERE p.localoid=$1::oid",
                  1, nullptr, dist_vals, nullptr, nullptr, 0));
              if (!dist || PQresultStatus(dist.get()) != PGRES_TUPLES_OK) {
                source = SourceError(dist.get(), d.product);
                error = ErrorState(dist.get());
                throw IOException("Greenplum distribution check failed");
              }
              if (PQntuples(dist.get()) == 1) {
                auto dist_attnums = ParseInt2Vector(PQgetvalue(dist.get(), 0, 0));
                if (std::find(dist_attnums.begin(), dist_attnums.end(), attnum) != dist_attnums.end()) {
                  error = "2BP01";
                  throw IOException("DDL cannot modify or drop Greenplum distribution key column");
                }
              }
            }
            if (d.intent.action == "alter_column_type") {
              if (!IsLosslessTypeWidening(typid, typmod, d.intent.column_specs[0], d.product)) {
                error = "0A000";
                throw IOException("DDL column type conversion is not lossless widening");
              }
            }
          }
        }
      }
    }

    if (!noop) {
      stage = "source_execute";
      sent = true;
      auto result = Wrap(PQexec(raw, d.intent.sql.c_str()));
      if (!OK(result.get())) {
        source=SourceError(result.get(), d.product); error = ErrorState(result.get());
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
        source=SourceError(commit.get(), d.product); error = ErrorState(commit.get());
        throw IOException("DDL commit outcome unavailable");
      }
      transaction = false;
      status = "COMMITTED";
      stage.clear();
    }
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
  if(d.diagnostics) out.SetValue(5,0,Value(source.JSON()));
  out.SetValue(0, 0, Value(d.intent.operation));
  out.SetValue(1, 0, Value(status));
  out.SetValue(2, 0, Value(error));
  out.SetValue(3, 0, Value(stage));
  out.SetValue(4, 0, Value::BIGINT(0));
}
} // namespace
void RegisterPostgresGuardedDDL(ExtensionLoader &loader) {
  auto register_function = [&](TableFunction function) {function.named_parameters["source_diagnostics"]=LogicalType::BOOLEAN;loader.RegisterFunction(function);};
  register_function(TableFunction("postgres_guarded_ddl_v5",
                                  {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::ANY},
                                  Scan, BindV5, Init));
  register_function(TableFunction("postgres_guarded_ddl_v6",
                                  {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::ANY},
                                  Scan, BindV6, Init));
  register_function(TableFunction("greenplum_guarded_ddl_v1",
                                  {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::ANY},
                                  Scan, BindGreenplumV1, Init));
  register_function(TableFunction("kingbasees_guarded_ddl_v1",
                                  {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::VARCHAR, LogicalType::VARCHAR,
                                   LogicalType::ANY},
                                  Scan, BindKingbaseV1, Init));
  register_function(TableFunction(
      "postgres_guarded_ddl_scoped_v2",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindScopedV2, Init));
  register_function(TableFunction(
      "postgres_guarded_ddl_scoped_v3",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindScopedV3, Init));
  register_function(TableFunction(
      "postgres_guarded_ddl_scoped_v4",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindScopedV4, Init));
  register_function(TableFunction(
      "postgres_guarded_ddl_scoped_v6",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindScopedV6, Init));
  register_function(TableFunction(
      "postgres_guarded_ddl_v2",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
       LogicalType::VARCHAR, LogicalType::ANY},
      Scan, Bind, Init));
  register_function(TableFunction(
      "postgres_guarded_ddl_v3",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
       LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindV3, Init));
  register_function(TableFunction(
      "postgres_guarded_ddl_v4",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
       LogicalType::VARCHAR, LogicalType::ANY},
      Scan, BindV4, Init));
}
} // namespace duckdb
