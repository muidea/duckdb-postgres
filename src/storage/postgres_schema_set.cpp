#include "storage/postgres_schema_set.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "storage/postgres_catalog.hpp"
#include "storage/postgres_index_set.hpp"
#include "storage/postgres_table_set.hpp"
#include "storage/postgres_transaction.hpp"
#include "storage/postgres_type_set.hpp"

namespace duckdb {

PostgresSchemaSet::PostgresSchemaSet(Catalog &catalog, string schema_to_load_p)
    : PostgresCatalogSet(catalog, false), schema_to_load(std::move(schema_to_load_p)) {
}

string PostgresSchemaSet::GetInitializeQuery(const string &schema) {
	string base_query = R"(
SELECT oid, nspname
FROM pg_namespace
${CONDITION}
ORDER BY oid;
)";
	string condition;
	if (!schema.empty()) {
		condition += "WHERE pg_namespace.nspname=" + KeywordHelper::WriteQuoted(schema);
	}
	return StringUtil::Replace(base_query, "${CONDITION}", condition);
}

void PostgresSchemaSet::LoadEntries(PostgresTransaction &transaction) {
	// Schema discovery must not retain a database-wide PGresult through
	// unconsumed (including empty) schema slices. Each category loads on demand.
	auto result = transaction.Query(GetInitializeQuery(schema_to_load));
	auto rows = result->Count();
	for (idx_t row = 0; row < rows; row++) {
		auto schema_name = result->GetString(row, 1);
		CreateSchemaInfo info;
		info.schema = schema_name;
		info.internal = PostgresSchemaEntry::SchemaIsInternal(schema_name);
		auto schema = make_shared_ptr<PostgresSchemaEntry>(catalog, info);
		CreateEntry(transaction, std::move(schema));
	}
}

optional_ptr<CatalogEntry> PostgresSchemaSet::CreateSchema(PostgresTransaction &transaction, CreateSchemaInfo &info) {
	string create_sql = "CREATE SCHEMA ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		create_sql += " IF NOT EXISTS";
	}
	create_sql += KeywordHelper::WriteQuoted(info.schema, '"');
	transaction.Query(create_sql);
	auto info_copy = info.Copy();
	info.internal = PostgresSchemaEntry::SchemaIsInternal(info_copy->schema);
	auto schema_entry = make_shared_ptr<PostgresSchemaEntry>(catalog, info_copy->Cast<CreateSchemaInfo>());
	return CreateEntry(transaction, std::move(schema_entry));
}

} // namespace duckdb
