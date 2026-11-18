//===----------------------------------------------------------------------===//
//                         DuckDB
//
// postgres_filter_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

namespace duckdb {

class PostgresFilterPushdown {
public:
	static string TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
	                               const vector<string> &names, const vector<string> &casts);
	static string TransformComparison(ExpressionType type);
	static string TransformFilter(string &column_name, TableFilter &filter, column_t column_id);
};

} // namespace duckdb
