#pragma once

#include <cstdint>
#include <string>

#include "postgres_type_oids.hpp"

namespace duckdb {

inline bool IsKingbaseOracleDateMetadata(const std::string &type_name, const std::string &type_schema, uint32_t type_oid,
                                         int16_t type_len) {
	if (type_name != "date") {
		return false;
	}
	if (type_schema == "sys" && (type_oid == KINGBASE_SYSDATEOID || type_len == 8)) {
		return true;
	}
	return type_schema.empty() && type_oid == KINGBASE_SYSDATEOID;
}

inline bool IsKingbaseOracleDateArrayMetadata(const std::string &type_name, const std::string &type_schema,
                                              uint32_t type_oid, uint32_t elem_oid) {
	if (type_name != "_date") {
		return false;
	}
	if (type_oid == KINGBASE_SYSDATEARRAYOID || elem_oid == KINGBASE_SYSDATEOID) {
		return true;
	}
	return type_schema == "sys" && elem_oid == KINGBASE_SYSDATEOID;
}

// Converts PostgreSQL ISO trailing " BC" ("0001-01-01 BC" /
// "0001-01-01 12:34:56+00 BC") to DuckDB's "(BC)" placement.
inline std::string NormalizePostgresBCText(const std::string &text, bool is_date_only) {
	if (text.size() < 4 || text.compare(text.size() - 3, 3, " BC") != 0) {
		return text;
	}
	std::string core = text.substr(0, text.size() - 3);
	if (is_date_only) {
		return core + " (BC)";
	}
	auto sep = core.find_first_of(" T");
	if (sep == std::string::npos) {
		return core + " (BC)";
	}
	return core.substr(0, sep) + " (BC)" + core.substr(sep);
}

// Converts DuckDB's "(BC)" formatting back to PostgreSQL trailing " BC".
inline std::string NormalizeDuckDBBCToPostgres(const std::string &text) {
	auto pos = text.find(" (BC)");
	if (pos == std::string::npos) {
		return text;
	}
	return text.substr(0, pos) + text.substr(pos + 5) + " BC";
}

} // namespace duckdb
