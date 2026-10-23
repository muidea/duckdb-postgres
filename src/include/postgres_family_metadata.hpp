#pragma once

#include "duckdb/common/string_util.hpp"
#include "postgres_crud_metadata.hpp"

namespace duckdb {

inline constexpr const char *POSTGRES_CRUD_PRODUCT = "postgres";
inline constexpr const char *GREENPLUM_CRUD_PRODUCT = "greenplum";
inline constexpr const char *KINGBASE_CRUD_PRODUCT = "kingbasees";

// These are separate source contracts. PostgreSQL wire compatibility alone is
// never evidence of equivalent catalogs, storage or session semantics.
static string PostgresFamilyMetadataSQL(const string &product) {
	auto sql = string(FQE_POSTGRES_METADATA_SQL);
	if (product == GREENPLUM_CRUD_PRODUCT) {
		sql = StringUtil::Replace(sql, "c.relispartition", "false");
		sql = StringUtil::Replace(sql, "c.relrowsecurity", "false");
		sql = StringUtil::Replace(sql, "c.relforcerowsecurity", "false");
		sql = StringUtil::Replace(sql, " AND x.conparentid=0", "");
		sql = StringUtil::Replace(sql, "x.indnkeyatts", "x.indnatts");
		sql = StringUtil::Replace(sql, "a.attidentity", "''::pg_catalog.text");
		sql = StringUtil::Replace(sql, "a.attgenerated", "''::pg_catalog.text");
		sql = StringUtil::Replace(sql, "co.collisdeterministic", "(co.oid IS NOT NULL)");
		// Heap storage only. Fingerprint the complete distribution policy and
		// its catalog revision; append-only/external/replicated tables need
		// their own execution and failure contracts.
		sql = StringUtil::Replace(sql, "'database', pg_catalog.current_database(),",
		    string("'product','") + GREENPLUM_CRUD_PRODUCT + "','product_facts',pg_catalog.json_build_object("
		    "'encoding',pg_catalog.current_setting('server_encoding'),"
		    "'storage',c.relstorage,'distribution',(SELECT pg_catalog.row_to_json(p)::pg_catalog.text "
		    "|| ':' || p.xmin::pg_catalog.text FROM pg_catalog.gp_distribution_policy p "
		    "WHERE p.localoid=c.oid AND p.policytype='p'))::pg_catalog.text,"
		    "'database',pg_catalog.current_database(),");
	} else if (product == KINGBASE_CRUD_PRODUCT) {
		sql = StringUtil::Replace(sql, "'database', pg_catalog.current_database(),",
		    string("'product','") + KINGBASE_CRUD_PRODUCT + "','product_facts',pg_catalog.json_build_object("
		    "'encoding',pg_catalog.current_setting('server_encoding'),"
		    "'mode',pg_catalog.current_setting('database_mode'))::pg_catalog.text,"
		    "'database',pg_catalog.current_database(),");
	}
	return sql;
}

} // namespace duckdb
