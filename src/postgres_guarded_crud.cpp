#include "duckdb.hpp"
#include "guarded_crud_deadline.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/postgres_catalog.hpp"
#include "postgres_family_metadata.hpp"

extern "C" {
PGresult *PQfqeBindPortal(PGconn *, int, const char *const *);
PGresult *PQfqeExecutePortal(PGconn *);
}

namespace duckdb {
namespace {
struct Data : TableFunctionData {
    PostgresCatalog &catalog;
    string schema, table, expected, version, sql, product;
    vector<Value> params;
    bool execute;
    bool returning = false;
    string returning_name;
    int64_t returning_oid=0;
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
    if (!db || db->GetCatalog().GetCatalogType() != POSTGRES_CRUD_PRODUCT) throw BinderException("guarded PostgreSQL catalog unavailable");
    auto data = make_uniq<Data>(db->GetCatalog().Cast<PostgresCatalog>(), in.inputs.size() == 6);
    auto &function_name = in.table_function.name;
    data->product = function_name.substr(0, function_name.find('_'));
    if (data->product != POSTGRES_CRUD_PRODUCT && data->product != GREENPLUM_CRUD_PRODUCT && data->product != KINGBASE_CRUD_PRODUCT)
        throw BinderException("guarded source contract unavailable");
    data->returning = function_name == "postgres_guarded_insert_returning_v1";
    if (data->returning) {
        auto name=in.named_parameters.find("returning_name"), oid=in.named_parameters.find("returning_oid");
        if (name==in.named_parameters.end() || oid==in.named_parameters.end() || name->second.IsNull() || oid->second.IsNull()) throw BinderException("returned column unavailable");
        data->returning_name=name->second.GetValue<string>(); data->returning_oid=oid->second.GetValue<int64_t>();
        if (data->returning_name.empty() || (data->returning_oid!=20 && data->returning_oid!=21 && data->returning_oid!=23 && data->returning_oid!=2950)) throw BinderException("returned type unsupported");
    }
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
    if(data->returning) { names.push_back("returned_rows");types.push_back(LogicalType::VARCHAR); }
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
    string returned_rows="[";
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
            string(PQgetvalue(identity.get(),0,2)) != "off")
            throw IOException("guarded identity unavailable");
        string identity_text = PQgetvalue(identity.get(),0,1);
        if ((d.product == POSTGRES_CRUD_PRODUCT && (identity_text.find("PostgreSQL ") != 0 || identity_text.find("Greenplum") != string::npos)) ||
            (d.product == GREENPLUM_CRUD_PRODUCT && (identity_text.find("PostgreSQL ") != 0 || identity_text.find("(Greenplum Database ") == string::npos)) ||
            (d.product == KINGBASE_CRUD_PRODUCT && identity_text.find("KingbaseES ") != 0))
            throw IOException("guarded source product mismatch");
        version = d.product == POSTGRES_CRUD_PRODUCT ? string(PQgetvalue(identity.get(),0,0)) : identity_text;
        if (d.execute && version != d.version) throw IOException("guarded backend changed");
        auto begin = Wrap(PQexec(raw, "BEGIN ISOLATION LEVEL READ COMMITTED")); if (!OK(begin.get())) throw IOException("guarded begin failed");
        string settings_sql = "SET LOCAL search_path=pg_catalog; SET LOCAL statement_timeout='25s'; SET LOCAL lock_timeout='5s'; SET LOCAL timezone='UTC'; SET LOCAL client_encoding='UTF8'; SET LOCAL datestyle='ISO,YMD'; SET LOCAL standard_conforming_strings=on";
        if (d.product != GREENPLUM_CRUD_PRODUCT) settings_sql += "; SET LOCAL row_security=on";
        auto settings = Wrap(PQexec(raw, settings_sql.c_str()));
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
            if (!OK(bound.get()) || PQnfields(bound.get()) != (d.returning?1:0) ||
                (d.returning && (string(PQfname(bound.get(),0))!=d.returning_name || PQftype(bound.get(),0)!=d.returning_oid))) { error = ErrorState(bound.get()); throw IOException("guarded portal unavailable"); }
        }
        const char *args[] = {d.schema.c_str(),d.table.c_str()};
        auto metadata_sql = PostgresFamilyMetadataSQL(d.product);
        auto meta = Wrap(PQexecParams(raw,metadata_sql.c_str(),2,nullptr,args,nullptr,nullptr,0));
        if (!meta || PQresultStatus(meta.get()) != PGRES_TUPLES_OK || PQntuples(meta.get()) != 1 || PQgetisnull(meta.get(),0,0)) throw IOException("guarded descriptor unavailable");
        descriptor = PQgetvalue(meta.get(),0,0);
        if (d.execute) {
            stage = "source_descriptor";
            if (descriptor != d.expected) { error = "0A000"; throw IOException("guarded descriptor changed"); }
            if (ctx.IsInterrupted()) { error = "57014"; throw InterruptException(); }
            stage = "source_execute"; sent = true;
            auto result = Wrap(PQfqeExecutePortal(raw));
            if (!result || PQresultStatus(result.get())!=(d.returning?PGRES_TUPLES_OK:PGRES_COMMAND_OK)) { error = ErrorState(result.get()); throw IOException("guarded execution failed"); }
            auto rows = PQcmdTuples(result.get()); if (!rows || !*rows) throw IOException("guarded count unavailable");
            affected = std::stoll(rows);
            if (d.returning) {
                if (PQnfields(result.get())!=1 || PQntuples(result.get())!=affected || affected<0 || affected>4096) { error="54000";throw IOException("returned rows exceed contract"); }
                for(int i=0;i<PQntuples(result.get());i++) {
                    if(i) returned_rows+=",";
                    returned_rows+="[";
                    if(PQgetisnull(result.get(),i,0)) returned_rows+="null";
                    else {
                        auto len=PQgetlength(result.get(),i,0);if(len<0 || len>64) { error="54000";throw IOException("returned identifier too large"); }
                        string value(PQgetvalue(result.get(),i,0),len);
                        if(value.find_first_not_of("0123456789abcdefABCDEF-")!=string::npos) { error="22021";throw IOException("returned identifier encoding unsupported"); }
                        returned_rows+="\""+value+"\"";
                    }
                    returned_rows+="]";
                }
                returned_rows+="]";
            }
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
    if(d.returning) out.SetValue(6,0,Value(status=="COMMITTED"?returned_rows:""));
}
}
void RegisterPostgresGuardedCRUD(ExtensionLoader &loader) {
    TableFunction returning("postgres_guarded_insert_returning_v1",vector<LogicalType>(6,LogicalType::VARCHAR),Scan,Bind,Init);
    returning.named_parameters["params"]=LogicalType::ANY;
    returning.named_parameters["returning_name"]=LogicalType::VARCHAR;
    returning.named_parameters["returning_oid"]=LogicalType::BIGINT;
    loader.RegisterFunction(returning);

    for (auto &product : {POSTGRES_CRUD_PRODUCT, GREENPLUM_CRUD_PRODUCT, KINGBASE_CRUD_PRODUCT}) {
        TableFunction metadata(string(product) + "_crud_metadata_v1",{LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR},Scan,Bind,Init);
        loader.RegisterFunction(metadata);
        TableFunction exec(string(product) + "_guarded_mutation_v1",vector<LogicalType>(6,LogicalType::VARCHAR),Scan,Bind,Init);
        exec.named_parameters["params"] = LogicalType::ANY;
        loader.RegisterFunction(exec);
    }
}
}
