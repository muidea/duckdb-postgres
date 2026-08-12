#include "storage/postgres_index_set.hpp"
#include "storage/postgres_schema_entry.hpp"
#include "storage/postgres_transaction.hpp"
#include "storage/postgres_optimizer.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "storage/postgres_catalog.hpp"
#include "postgres_scanner.hpp"

namespace duckdb {

struct PostgresOperators {
	reference_map_t<PostgresCatalog, vector<reference<LogicalGet>>> scans;
};

static void OptimizePostgresScanLimitPushdown(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &limit = op->Cast<LogicalLimit>();
		reference<LogicalOperator> child = *op->children[0];

		while (child.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
			child = *child.get().children[0];
		}

		if (child.get().type != LogicalOperatorType::LOGICAL_GET) {
			OptimizePostgresScanLimitPushdown(op->children[0]);
			return;
		}

		auto &get = child.get().Cast<LogicalGet>();
		if (!PostgresCatalog::IsPostgresScan(get.function.name)) {
			OptimizePostgresScanLimitPushdown(op->children[0]);
			return;
		}

		switch (limit.limit_val.Type()) {
		case LimitNodeType::CONSTANT_VALUE:
		case LimitNodeType::UNSET:
			break;
		default:
			// not a constant or unset limit
			OptimizePostgresScanLimitPushdown(op->children[0]);
			return;
		}
		switch (limit.offset_val.Type()) {
		case LimitNodeType::CONSTANT_VALUE:
		case LimitNodeType::UNSET:
			break;
		default:
			// not a constant or unset offset
			OptimizePostgresScanLimitPushdown(op->children[0]);
			return;
		}

		auto &bind_data = get.bind_data->Cast<PostgresBindData>();
		if (bind_data.max_threads != 1 || !bind_data.can_use_main_thread) {
			// cannot push down limit/offset if we are not using the main thread
			OptimizePostgresScanLimitPushdown(op->children[0]);
			return;
		}

		string generated_limit_clause = "";
		if (limit.limit_val.Type() != LimitNodeType::UNSET) {
			generated_limit_clause += " LIMIT " + to_string(limit.limit_val.GetConstantValue());
		}
		if (limit.offset_val.Type() != LimitNodeType::UNSET) {
			generated_limit_clause += " OFFSET " + to_string(limit.offset_val.GetConstantValue());
		}

		if (!generated_limit_clause.empty()) {
			bind_data.limit = generated_limit_clause;

			op = std::move(op->children[0]);
			return;
		}
	}

	for (auto &child : op->children) {
		OptimizePostgresScanLimitPushdown(child);
	}
}

void GatherPostgresScans(LogicalOperator &op, PostgresOperators &result) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		auto &table_scan = get.function;
		if (!PostgresCatalog::IsPostgresScan(table_scan.name)) {
			// not a postgres scan - skip
			return;
		}
		auto &bind_data = get.bind_data->Cast<PostgresBindData>();
		auto catalog = bind_data.GetCatalog();
		if (!catalog) {
			// "postgres_scan" functions are fully independent - we can always stream them
			return;
		}
		result.scans[*catalog].push_back(get);
	}
	// recurse into children
	for (auto &child : op.children) {
		GatherPostgresScans(*child, result);
	}
}

void PostgresOptimizer::Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	// look at query plan and check if we can find LIMIT/OFFSET to pushdown
	OptimizePostgresScanLimitPushdown(plan);
	// look at the query plan and check if we can enable streaming query scans
	PostgresOperators operators;
	GatherPostgresScans(*plan, operators);
	if (operators.scans.empty()) {
		// no scans
		return;
	}
	for (auto &entry : operators.scans) {
		auto &catalog = entry.first;
		if (entry.second.size() == 1) {
			auto &bind_data = entry.second[0].get().bind_data->Cast<PostgresBindData>();
			// A single scan can stream through the transaction connection.
			bind_data.requires_materialization = false;
			bind_data.can_use_main_thread = true;
			continue;
		}

		vector<reference<LogicalGet>> streamable_scans;
		for (auto &scan : entry.second) {
			auto &bind_data = scan.get().bind_data->Cast<PostgresBindData>();
			if (bind_data.max_threads > 1 && bind_data.read_only) {
				streamable_scans.push_back(scan);
			} else {
				bind_data.requires_materialization = true;
				bind_data.can_use_main_thread = true;
			}
		}

		if (streamable_scans.empty()) {
			continue;
		}

		// Multi-scan plans cannot share the transaction connection. Reserve that slot for snapshot
		// coordination and divide the remaining pool capacity across the streaming scan operators.
		// If there is not one connection per operator, materialize instead of exceeding the configured limit.
		auto connection_limit = catalog.get().GetConnectionPool().GetMaximumConnections();
		if (connection_limit <= streamable_scans.size()) {
			for (auto &scan : streamable_scans) {
				auto &bind_data = scan.get().bind_data->Cast<PostgresBindData>();
				bind_data.requires_materialization = true;
				bind_data.can_use_main_thread = true;
			}
			continue;
		}

		auto max_threads_per_scan = (connection_limit - 1) / streamable_scans.size();
		for (auto &scan : streamable_scans) {
			auto &bind_data = scan.get().bind_data->Cast<PostgresBindData>();
			bind_data.requires_materialization = false;
			bind_data.can_use_main_thread = false;
			bind_data.max_threads = MinValue(bind_data.max_threads, max_threads_per_scan);
		}
	}
}

} // namespace duckdb
