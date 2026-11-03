#include "duckdb.hpp"

#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "postgres_scanner.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/postgres_catalog.hpp"
#include "storage/postgres_transaction.hpp"
#include "storage/postgres_schema_entry.hpp"

namespace duckdb {

static unique_ptr<FunctionData> PGQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PostgresBindData>(context);

	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to postgres_query cannot be NULL");
	}

	// look up the database to query
	auto db_name = input.inputs[0].GetValue<string>();
	auto &db_manager = DatabaseManager::Get(context);
	auto db = db_manager.GetDatabase(context, db_name);
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\" referenced in postgres_query", db_name);
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != "postgres") {
		throw BinderException("Attached database \"%s\" does not refer to a Postgres database", db_name);
	}
	auto &pg_catalog = catalog.Cast<PostgresCatalog>();
	auto &transaction = Transaction::Get(context, catalog).Cast<PostgresTransaction>();
	auto sql = input.inputs[1].GetValue<string>();
	// strip any trailing semicolons
	StringUtil::RTrim(sql);
	while (!sql.empty() && sql.back() == ';') {
		sql = sql.substr(0, sql.size() - 1);
		StringUtil::RTrim(sql);
	}

	bool use_transaction = true;
	for (auto &kv : input.named_parameters) {
		if (kv.first == "use_transaction") {
			use_transaction = BooleanValue::Get(kv.second);
		}
	}
	result->use_transaction = use_transaction;

	auto &con = use_transaction ? transaction.GetConnection() : transaction.GetConnectionWithoutTransaction();

	auto conn = con.GetConn();
	// prepare execution of the query to figure out the result types and names
	auto prepared = PQprepare(conn, "", sql.c_str(), 0, nullptr);
	PostgresResult prepared_wrapper(prepared);
	if (!prepared) {
		throw BinderException("Failed to prepare query \"%s\" (no result returned): %s", sql, PQerrorMessage(conn));
	}
	if (PQresultStatus(prepared) != PGRES_COMMAND_OK) {
		throw BinderException("Failed to prepare query \"%s\": %s", sql, PQresultErrorMessage(prepared));
	}
	// use describe_prepared
	auto describe_prepared = PQdescribePrepared(conn, "");
	PostgresResult describe_wrapper(describe_prepared);
	if (!describe_prepared || PQresultStatus(describe_prepared) != PGRES_COMMAND_OK) {
		auto extended_err = describe_prepared ? PQresultErrorMessage(describe_prepared) : PQerrorMessage(conn);
		throw BinderException("Failed to describe prepared statement: %s", extended_err);
	}
	auto nfields = PQnfields(describe_prepared);
	if (nfields <= 0) {
		throw BinderException("No fields returned by query \"%s\" - the query must be a SELECT statement that returns "
		                      "at least one column",
		                      sql);
	}
	// Describe never executes the user query. Resolve declared dimensions and
	// real type names using only catalog identities supplied by libpq, on the
	// same source connection. Expressions have no reliable declared shape.
	string fields;
	for (idx_t c = 0; c < nfields; c++) {
		if (c) { fields += ","; }
		fields += "(" + to_string(c) + "," + to_string(PQftype(describe_prepared, c)) +
		          "::oid," + to_string(PQftable(describe_prepared, c)) + "::oid," +
		          to_string(PQftablecol(describe_prepared, c)) + ")";
	}
	auto metadata = con.Query(
	    "SELECT f.i, t.typname, n.nspname, coalesce(a.attndims,0), CASE WHEN t.typelem<>0 AND t.typlen=-1 THEN 1 ELSE 0 END "
	    "FROM (VALUES " + fields + ") AS f(i,type_oid,relation_oid,attribute_number) "
	    "JOIN pg_catalog.pg_type t ON t.oid=f.type_oid "
	    "JOIN pg_catalog.pg_namespace n ON n.oid=t.typnamespace "
	    "LEFT JOIN pg_catalog.pg_attribute a ON a.attrelid=f.relation_oid "
	    "AND a.attnum=f.attribute_number AND NOT a.attisdropped ORDER BY f.i");
	if (metadata->Count() != idx_t(nfields)) {
		throw BinderException("Could not resolve all postgres_query result types");
	}
	for (idx_t c = 0; c < idx_t(nfields); c++) {
		PostgresType postgres_type;
		postgres_type.oid = PQftype(describe_prepared, c);
		PostgresTypeData type_data;
		type_data.type_name = metadata->GetString(c, 1);
		type_data.type_modifier = PQfmod(describe_prepared, c);
		type_data.array_dimensions = metadata->GetInt64(c, 3);
		auto type_schema_name = metadata->GetString(c, 2);
		optional_ptr<PostgresSchemaEntry> type_schema;
		// Built-in types need no schema load. Custom types use the same catalog
		// resolver as attached scans (including enums, domains and composites).
		if (type_schema_name != "pg_catalog") {
			auto schema = pg_catalog.GetSchema(context, type_schema_name, OnEntryNotFound::RETURN_NULL);
			if (schema) { type_schema = &schema->Cast<PostgresSchemaEntry>(); }
		}
		LogicalType converted_type;
		if (metadata->GetInt64(c, 4) != 0 && type_data.array_dimensions == 0) {
			// No sampling/LIMIT 1: ARRAY expressions (and unknown shapes) are
			// returned as exact source text, with an honest VARCHAR descriptor.
			postgres_type.info = PostgresTypeAnnotation::CAST_TO_VARCHAR;
			converted_type = LogicalType::VARCHAR;
		} else {
			converted_type = PostgresUtils::TypeToLogicalType(&transaction, type_schema, type_data, postgres_type);
		}
		result->postgres_types.push_back(postgres_type);
		return_types.emplace_back(converted_type);
		names.emplace_back(PQfname(describe_prepared, c));
	}

	// set up the bind data
	result->SetCatalog(pg_catalog);
	result->dsn = con.GetDSN();
	result->types = return_types;
	result->names = names;
	result->read_only = false;
	result->SetTablePages(0);
	result->sql = std::move(sql);
	return std::move(result);
}

PostgresQueryFunction::PostgresQueryFunction()
    : TableFunction("postgres_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, PGQueryBind) {
	named_parameters["use_transaction"] = LogicalType::BOOLEAN;
	PostgresScanFunction scan_function;
	init_global = scan_function.init_global;
	init_local = scan_function.init_local;
	function = scan_function.function;
	projection_pushdown = true;
	global_initialization = TableFunctionInitialization::INITIALIZE_ON_SCHEDULE;
}
} // namespace duckdb
