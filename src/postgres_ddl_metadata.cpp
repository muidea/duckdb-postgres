#include "duckdb.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_crud_deadline.hpp"
#include "storage/postgres_catalog.hpp"

namespace duckdb {
namespace {
struct DDLMetadataData : TableFunctionData {
  PostgresCatalog &catalog;
  string schema, name, kind;
  bool describe;
  DDLMetadataData(PostgresCatalog &c, bool d) : catalog(c), describe(d) {}
};
struct DDLMetadataState : GlobalTableFunctionState {
  bool done = false;
};
static unique_ptr<GlobalTableFunctionState> Init(ClientContext &,
                                                 TableFunctionInitInput &) {
  return make_uniq<DDLMetadataState>();
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx,
                                     TableFunctionBindInput &in,
                                     vector<LogicalType> &types,
                                     vector<string> &names) {
  for (auto &v : in.inputs)
    if (v.IsNull())
      throw BinderException("DDL metadata input is incomplete");
  auto db = DatabaseManager::Get(ctx).GetDatabase(
      ctx, in.inputs[0].GetValue<string>());
  if (!db || db->GetCatalog().GetCatalogType() != "postgres")
    throw BinderException("DDL metadata catalog is unavailable");
  auto data = make_uniq<DDLMetadataData>(
      db->GetCatalog().Cast<PostgresCatalog>(), in.inputs.size() == 4);
  data->schema = in.inputs[1].GetValue<string>();
  if (data->describe) {
    data->kind = in.inputs[2].GetValue<string>();
    data->name = in.inputs[3].GetValue<string>();
  }
  for (auto &v : {data->schema, data->name})
    if (v.size() > 256 || v.find('\0') != string::npos)
      throw BinderException("DDL metadata identifier is invalid");
  if (data->schema.empty() ||
      (data->describe && (data->name.empty() ||
                          (data->kind != "table" && data->kind != "index"))))
    throw BinderException("DDL metadata target is invalid");
  names = {"sqlstate", "stage", "document"};
  types = vector<LogicalType>(3, LogicalType::VARCHAR);
  return std::move(data);
}
static const char *CapabilitiesSQL = R"SQL(
SELECT pg_catalog.json_build_object(
 'contract_version',1,'product','postgres','source_version',pg_catalog.current_setting('server_version'),
 'database',pg_catalog.current_database(),'database_identity',d.oid::text,'schema', $1::text,
 'namespace_identity',coalesce(n.oid::text,''),'namespace_present',n.oid IS NOT NULL,
 'source_read_only',pg_catalog.current_setting('transaction_read_only')='on',
 'session_identity',pg_catalog.json_build_object('user',current_user,'encoding',pg_catalog.current_setting('server_encoding'),
   'collation',d.datcollate,'ctype',d.datctype,'standard_strings',pg_catalog.current_setting('standard_conforming_strings')),
 'namespace_usage',CASE WHEN n.oid IS NULL THEN false ELSE pg_catalog.has_schema_privilege(n.oid,'USAGE') END,
 'namespace_create',CASE WHEN n.oid IS NULL THEN false ELSE pg_catalog.has_schema_privilege(n.oid,'CREATE') END,
 'visibility','CATALOG_READ','executor_available',false,'identity_protection','UNVERIFIED')::text
FROM pg_catalog.pg_database d LEFT JOIN pg_catalog.pg_namespace n ON n.nspname=$1 WHERE d.datname=pg_catalog.current_database()
)SQL";
// A bounded observation, deliberately NOT an execution identity/DDL lock proof.
// Presence is exact, but this first snapshot does not certify all dependencies.
static const char *DescribeSQL = R"SQL(
SELECT pg_catalog.json_build_object(
 'contract_version',1,'product','postgres','source_version',pg_catalog.current_setting('server_version'),
 'database',pg_catalog.current_database(),'database_identity',d.oid::text,'schema',$1::text,'name',$2::text,'kind',$3::text,
 'namespace_identity',coalesce(n.oid::text,''),'namespace_present',n.oid IS NOT NULL,
 'presence',CASE WHEN c.oid IS NULL THEN 'ABSENT' ELSE 'PRESENT' END,
 'object_identity',coalesce(c.oid::text,''),'observed_kind',coalesce(c.relkind::text,''),
 'owning_table',coalesce(t.relname,''),'table_identity',coalesce(t.oid::text,''),
 'metadata_complete',false,'identity_protection','UNVERIFIED',
 'columns',coalesce((SELECT pg_catalog.json_agg(x ORDER BY x.attnum) FROM (
   SELECT a.attnum,a.attname,a.atttypid::text,a.atttypmod,a.attnotnull,a.attisdropped,a.attidentity,a.attgenerated,a.attcollation::text
   FROM pg_catalog.pg_attribute a WHERE a.attrelid=coalesce(t.oid,c.oid) AND a.attnum>0 ORDER BY a.attnum LIMIT 257
 )x),'[]'::json),
 'indexes',coalesce((SELECT pg_catalog.json_agg(x ORDER BY x.indexrelid) FROM (
   SELECT i.indexrelid::text,i.indrelid::text,i.indisunique,i.indisprimary,i.indisvalid,i.indisready,i.indkey::text,
          i.indcollation::text,i.indclass::text,i.indoption::text
   FROM pg_catalog.pg_index i WHERE i.indrelid=coalesce(t.oid,c.oid) ORDER BY i.indexrelid LIMIT 257
 )x),'[]'::json),
 'constraints',coalesce((SELECT pg_catalog.json_agg(x ORDER BY x.oid) FROM (
   SELECT k.oid::text,k.conname,k.contype::text,k.conrelid::text,k.confrelid::text,k.conindid::text,k.conkey,k.confkey
   FROM pg_catalog.pg_constraint k WHERE k.conrelid=coalesce(t.oid,c.oid) OR k.confrelid=coalesce(t.oid,c.oid)
   ORDER BY k.oid LIMIT 257
 )x),'[]'::json))::text
FROM pg_catalog.pg_database d
LEFT JOIN pg_catalog.pg_namespace n ON n.nspname=$1
LEFT JOIN pg_catalog.pg_class c ON c.relnamespace=n.oid AND c.relname=$2
LEFT JOIN pg_catalog.pg_index ix ON ix.indexrelid=c.oid AND $3='index'
LEFT JOIN pg_catalog.pg_class t ON t.oid=ix.indrelid
WHERE d.datname=pg_catalog.current_database()
)SQL";
static void Scan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
  auto &state = in.global_state->Cast<DDLMetadataState>();
  if (state.done)
    return;
  state.done = true;
  auto &d = in.bind_data->Cast<DDLMetadataData>();
  string error, stage = "source_identity", document;
  PostgresPoolConnection pooled;
  unique_ptr<GuardedCRUDDeadline> deadline;
  using Result = unique_ptr<PGresult, decltype(&PQclear)>;
  auto wrap = [](PGresult *r) { return Result(r, PQclear); };
  try {
    if (ctx.IsInterrupted())
      throw InterruptException();
    pooled = d.catalog.GetConnectionPool().GetConnection();
    auto &con = pooled.GetConnection();
    auto raw = con.GetConn();
    deadline = make_uniq<GuardedCRUDDeadline>(ctx, PQsocket(raw), 5);
    if (PQtransactionStatus(raw) != PQTRANS_IDLE)
      throw IOException("DDL metadata session is not idle");
    auto identity = wrap(PQexec(raw, "SELECT pg_catalog.version()"));
    if (!identity || PQresultStatus(identity.get()) != PGRES_TUPLES_OK ||
        PQntuples(identity.get()) != 1)
      throw IOException("DDL metadata identity unavailable");
    string product = PQgetvalue(identity.get(), 0, 0);
    if (product.find("PostgreSQL ") != 0 ||
        product.find("Greenplum") != string::npos) {
      error = "0A000";
      throw IOException("DDL metadata adapter unavailable");
    }
    // Read-only, fixed metadata SQL. No user DDL, no resets/commits of a
    // borrowed transaction.
    stage = "source_metadata";
    const char *args[] = {d.schema.c_str(), d.name.c_str(), d.kind.c_str()};
    auto result = wrap(
        PQexecParams(raw, d.describe ? DescribeSQL : CapabilitiesSQL,
                     d.describe ? 3 : 1, nullptr, args, nullptr, nullptr, 0));
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
      const char *s =
          result ? PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) : nullptr;
      error = s && strlen(s) == 5 ? s : "08006";
      throw IOException("DDL metadata unavailable");
    }
    if (PQntuples(result.get()) != 1 || PQgetisnull(result.get(), 0, 0) ||
        PQgetlength(result.get(), 0, 0) > 1024 * 1024) {
      error = "54000";
      throw IOException("DDL metadata bound exceeded");
    }
    document = PQgetvalue(result.get(), 0, 0);
    stage.clear();
  } catch (...) {
    if (error.empty())
      error = ctx.IsInterrupted() ? "57014" : "08006";
    document.clear();
  }
  if (pooled.HasConnection()) {
    deadline.reset();
    pooled.GetConnection().Close();
  }
  out.SetCardinality(1);
  out.SetValue(0, 0, Value(error));
  out.SetValue(1, 0, Value(stage));
  out.SetValue(2, 0, Value(document));
}
} // namespace
void RegisterPostgresDDLMetadata(ExtensionLoader &loader) {
  loader.RegisterFunction(TableFunction(
      "postgres_ddl_capabilities_v1",
      vector<LogicalType>(2, LogicalType::VARCHAR), Scan, Bind, Init));
  loader.RegisterFunction(TableFunction(
      "postgres_ddl_describe_v1", vector<LogicalType>(4, LogicalType::VARCHAR),
      Scan, Bind, Init));
}
} // namespace duckdb
