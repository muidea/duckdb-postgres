#include "postgres_temporal_text.hpp"
#include <iostream>

int main() {
	using duckdb::IsKingbaseOracleDateArrayMetadata;
	using duckdb::IsKingbaseOracleDateMetadata;
	using duckdb::NormalizeDuckDBBCToPostgres;
	using duckdb::NormalizePostgresBCText;

	// NormalizePostgresBCText: DATE cases
	if (NormalizePostgresBCText("0001-01-01 BC", true) != "0001-01-01 (BC)") {
		std::cerr << "DATE 0001-01-01 BC mismatch\n";
		return 1;
	}
	if (NormalizePostgresBCText("4713-11-24 BC", true) != "4713-11-24 (BC)") {
		std::cerr << "DATE 4713-11-24 BC mismatch\n";
		return 1;
	}
	if (NormalizePostgresBCText("2026-01-01", true) != "2026-01-01" ||
	    NormalizePostgresBCText("infinity", true) != "infinity" ||
	    NormalizePostgresBCText("-infinity", true) != "-infinity") {
		std::cerr << "AD/infinity DATE mismatch\n";
		return 2;
	}

	// NormalizePostgresBCText: TIMESTAMP/TIMESTAMPTZ cases
	if (NormalizePostgresBCText("0001-01-01 12:34:56 BC", false) != "0001-01-01 (BC) 12:34:56") {
		std::cerr << "TIMESTAMP BC mismatch\n";
		return 3;
	}
	if (NormalizePostgresBCText("0001-01-01 12:34:56.123456+00 BC", false) != "0001-01-01 (BC) 12:34:56.123456+00") {
		std::cerr << "TIMESTAMPTZ BC mismatch\n";
		return 4;
	}
	if (NormalizePostgresBCText("0001-01-01T12:34:56+00 BC", false) != "0001-01-01 (BC)T12:34:56+00") {
		std::cerr << "ISO T TIMESTAMPTZ BC mismatch\n";
		return 5;
	}
	if (NormalizePostgresBCText("2026-01-02 03:04:05.123456+00", false) != "2026-01-02 03:04:05.123456+00") {
		std::cerr << "AD TIMESTAMP mismatch\n";
		return 6;
	}

	// NormalizeDuckDBBCToPostgres
	if (NormalizeDuckDBBCToPostgres("0001-01-01 (BC)") != "0001-01-01 BC" ||
	    NormalizeDuckDBBCToPostgres("0001-01-01 (BC) 12:34:56") != "0001-01-01 12:34:56 BC" ||
	    NormalizeDuckDBBCToPostgres("0001-01-01 (BC) 12:34:56.123456+00") != "0001-01-01 12:34:56.123456+00 BC" ||
	    NormalizeDuckDBBCToPostgres("2026-01-02 03:04:05+00") != "2026-01-02 03:04:05+00" ||
	    NormalizeDuckDBBCToPostgres("infinity") != "infinity") {
		std::cerr << "NormalizeDuckDBBCToPostgres mismatch\n";
		return 7;
	}

	// KingbaseES sys.date / sys._date metadata classification
	if (!IsKingbaseOracleDateMetadata("date", "sys", 8020, 8) ||
	    !IsKingbaseOracleDateMetadata("date", "sys", 0, 8) ||
	    IsKingbaseOracleDateMetadata("date", "pg_catalog", 1082, 4) ||
	    IsKingbaseOracleDateMetadata("date", "sys", 9999, 4) ||
	    !IsKingbaseOracleDateArrayMetadata("_date", "sys", 8021, 8020) ||
	    IsKingbaseOracleDateArrayMetadata("_date", "pg_catalog", 1182, 1082)) {
		std::cerr << "KingbaseES sys.date classification mismatch\n";
		return 8;
	}

	std::cout << "PostgreSQL temporal text helper passed\n";
	return 0;
}
