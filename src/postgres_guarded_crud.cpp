#include "duckdb.hpp"
#include "guarded_crud_deadline.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/postgres_catalog.hpp"
#include "postgres_crud_metadata.hpp"

extern "C" {
PGresult *PQfqeBindPortal(PGconn *, int, const char *const *);
PGresult *PQfqeExecutePortal(PGconn *);
}

namespace duckdb {
namespace {
struct Data : TableFunctionData {
    PostgresCatalog &catalog;
    string schema, table, expected, version, sql;
    vector<Value> params;
    bool execute;
    Data(PostgresCatalog &c, bool exec) : catalog(c), execute(exec) {}
};
struct State : GlobalTableFunctionState { bool done = false; };
static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<State>();
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx, TableFunctionBindInput &in,
                                    vector<LogicalType> &types, vector<string> &names) {
    for (auto &v : in.inputs) if (v.IsNull()) throw BinderException("guarded PostgreSQL input is incomplete");
    auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, in.inputs[0].GetValue<string>());
    if (!db || db->GetCatalog().GetCatalogType() != "postgres") throw BinderException("guarded PostgreSQL catalog unavailable");
    auto data = make_uniq<Data>(db->GetCatalog().Cast<PostgresCatalog>(), in.inputs.size() == 6);
    data->schema = in.inputs[1].GetValue<string>(); data->table = in.inputs[2].GetValue<string>();
    if (data->execute) {
        data->expected = in.inputs[3].GetValue<string>(); data->version = in.inputs[4].GetValue<string>();
        data->sql = in.inputs[5].GetValue<string>();
        auto it = in.named_parameters.find("params");
        if (it != in.named_parameters.end()) data->params = StructValue::GetChildren(it->second);
    }
    names = {"status", "affected_rows", "sqlstate", "stage", "version", "descriptor"};
    types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR,
             LogicalType::VARCHAR, LogicalType::VARCHAR};
    return std::move(data);
}
using Result = unique_ptr<PGresult, decltype(&PQclear)>;
static Result Wrap(PGresult *r) { return Result(r, PQclear); }
static bool OK(PGresult *r) { return r && PQresultStatus(r) == PGRES_COMMAND_OK; }
static string ErrorState(PGresult *r) {
    auto state = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : nullptr;
    return state && strlen(state) == 5 ? state : "08006";
}
static void Scan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
    auto &state = in.global_state->Cast<State>(); if (state.done) return; state.done = true;
    auto &d = in.bind_data->Cast<Data>();
    string status = "CONFIRMED_NOT_EXECUTED", error, stage = "source_metadata", version, descriptor;
    int64_t affected = -1;
    bool sent = false, finished = false;
    PostgresPoolConnection pooled;
    unique_ptr<GuardedCRUDDeadline> deadline;
    try {
        if (ctx.IsInterrupted()) throw InterruptException();
        pooled = d.catalog.GetConnectionPool().GetConnection();
        auto &con = pooled.GetConnection(); auto raw = con.GetConn();
        deadline = make_uniq<GuardedCRUDDeadline>(ctx, PQsocket(raw));
        // No state from a query transaction is borrowed or committed.
        if (PQtransactionStatus(raw) != PQTRANS_IDLE) throw IOException("guarded PostgreSQL connection is not idle");
        auto reset = Wrap(PQexec(raw, "DISCARD ALL")); if (!OK(reset.get())) throw IOException("guarded reset failed");
        auto identity = Wrap(PQexec(raw, "SELECT current_setting('server_version'),version(),current_setting('transaction_read_only')"));
        if (!identity || PQresultStatus(identity.get()) != PGRES_TUPLES_OK || PQntuples(identity.get()) != 1 ||
            string(PQgetvalue(identity.get(),0,1)).find("PostgreSQL ") != 0 || string(PQgetvalue(identity.get(),0,2)) != "off")
            throw IOException("guarded identity unavailable");
        version = PQgetvalue(identity.get(),0,0);
        if (d.execute && version != d.version) throw IOException("guarded backend changed");
        auto begin = Wrap(PQexec(raw, "BEGIN ISOLATION LEVEL READ COMMITTED")); if (!OK(begin.get())) throw IOException("guarded begin failed");
        auto settings = Wrap(PQexec(raw, "SET LOCAL search_path=pg_catalog; SET LOCAL statement_timeout='25s'; SET LOCAL lock_timeout='5s'; SET LOCAL timezone='UTC'; SET LOCAL client_encoding='UTF8'; SET LOCAL row_security=on; SET LOCAL datestyle='ISO,YMD'"));
        if (!OK(settings.get())) throw IOException("guarded settings failed");
        if (d.execute) {
            auto lock = Wrap(PQexec(raw, ("LOCK TABLE ONLY " + KeywordHelper::WriteQuoted(d.schema, '"') + "." + KeywordHelper::WriteQuoted(d.table, '"') + " IN SHARE UPDATE EXCLUSIVE MODE").c_str()));
            if (!OK(lock.get())) { error = ErrorState(lock.get()); throw IOException("guarded lock failed"); }
            stage = "source_prepare";
            auto prepare = Wrap(PQprepare(raw, "fqe_guarded_statement", d.sql.c_str(), 0, nullptr));
            if (!OK(prepare.get())) { error = ErrorState(prepare.get()); throw IOException("guarded prepare failed"); }
            vector<string> text; text.reserve(d.params.size());
            for (auto &v : d.params) { text.push_back(v.IsNull() ? "" : v.ToString()); if (text.back().find('\0') != string::npos) throw IOException("unsupported text encoding"); }
            vector<const char *> params; for (idx_t i=0;i<d.params.size();i++) params.push_back(d.params[i].IsNull()?nullptr:text[i].c_str());
            auto bound = Wrap(PQfqeBindPortal(raw, params.size(), params.data()));
            if (!OK(bound.get()) || PQnfields(bound.get()) != 0) { error = ErrorState(bound.get()); throw IOException("guarded portal unavailable"); }
        }
        const char *args[] = {d.schema.c_str(),d.table.c_str()};
        auto meta = Wrap(PQexecParams(raw,FQE_POSTGRES_METADATA_SQL,2,nullptr,args,nullptr,nullptr,0));
        if (!meta || PQresultStatus(meta.get()) != PGRES_TUPLES_OK || PQntuples(meta.get()) != 1 || PQgetisnull(meta.get(),0,0)) throw IOException("guarded descriptor unavailable");
        descriptor = PQgetvalue(meta.get(),0,0);
        if (d.execute) {
            stage = "source_descriptor";
            if (descriptor != d.expected) { error = "0A000"; throw IOException("guarded descriptor changed"); }
            if (ctx.IsInterrupted()) { error = "57014"; throw InterruptException(); }
            stage = "source_execute"; sent = true;
            auto result = Wrap(PQfqeExecutePortal(raw));
            if (!OK(result.get())) { error = ErrorState(result.get()); throw IOException("guarded execution failed"); }
            auto rows = PQcmdTuples(result.get()); if (!rows || !*rows) throw IOException("guarded count unavailable");
            affected = std::stoll(rows);
            stage = "source_commit"; status = "COMMIT_STATUS_UNKNOWN"; finished = true;
            auto commit = Wrap(PQexec(raw,"COMMIT"));
            if (!OK(commit.get()) || string(PQcmdStatus(commit.get())) != "COMMIT") {
                error = ErrorState(commit.get());
            } else { status = "COMMITTED"; stage.clear(); }
        }
        if (!d.execute) {
            auto rollback = Wrap(PQexec(raw,"ROLLBACK")); if (!OK(rollback.get())) throw IOException("guarded cleanup failed");
            finished = true; stage.clear();
        }
    } catch (...) {
        if (error.empty()) error = ctx.IsInterrupted() ? "57014" : "08006";
    }
    if (pooled.HasConnection()) {
        auto &con = pooled.GetConnection();
        if (!finished && con.IsOpen()) {
            auto rollback = Wrap(PQexec(con.GetConn(),"ROLLBACK"));
            if (OK(rollback.get()) && PQstatus(con.GetConn()) == CONNECTION_OK) {
                if (sent) status = "CONFIRMED_ROLLED_BACK";
            } else { if (sent) status = "COMMIT_STATUS_UNKNOWN"; }
        }
        // Dedicated guarded sessions are never returned with local GUC/portal state.
        deadline.reset(); // Keep cleanup bounded; stop/join before the socket can be reused.
        con.Close();
    }
    out.SetCardinality(1);
    out.SetValue(0,0,Value(status)); out.SetValue(1,0,Value::BIGINT(affected)); out.SetValue(2,0,Value(error));
    out.SetValue(3,0,Value(stage)); out.SetValue(4,0,Value(version)); out.SetValue(5,0,Value(descriptor));
}
}
void RegisterPostgresGuardedCRUD(ExtensionLoader &loader) {
    TableFunction metadata("postgres_crud_metadata_v1",{LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR},Scan,Bind,Init);
    loader.RegisterFunction(metadata);
    TableFunction exec("postgres_guarded_mutation_v1",vector<LogicalType>(6,LogicalType::VARCHAR),Scan,Bind,Init);
    exec.named_parameters["params"] = LogicalType::ANY;
    loader.RegisterFunction(exec);
}
}
