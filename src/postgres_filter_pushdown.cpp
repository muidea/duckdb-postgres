#include "postgres_filter_pushdown.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

namespace duckdb {

string PostgresFilterPushdown::TransformComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "<>";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		throw NotImplementedException("Unsupported expression type");
	}
}

string TransformBlob(const string &val) {
	char const HEX_DIGITS[] = "0123456789ABCDEF";

	string result = "'\\x";
	for (idx_t i = 0; i < val.size(); i++) {
		uint8_t byte_val = static_cast<uint8_t>(val[i]);
		result += HEX_DIGITS[(byte_val >> 4) & 0xf];
		result += HEX_DIGITS[byte_val & 0xf];
	}
	result += "'::BYTEA";
	return result;
}

string TransformLiteral(const Value &val) {
	switch (val.type().id()) {
	case LogicalTypeId::BLOB:
		return TransformBlob(StringValue::Get(val));
	default:
		return KeywordHelper::WriteQuoted(val.ToString());
	}
}

static string TransformFilterExpr(const string &column_name, const Expression &expr, column_t column_id);

static string CreateConjunctionExpr(const string &column_name, const vector<unique_ptr<Expression>> &children,
                                    const string &op, column_t column_id) {
	vector<string> parts;
	for (auto &child : children) {
		auto part = TransformFilterExpr(column_name, *child, column_id);
		if (part.empty()) {
			return string();
		}
		parts.push_back(std::move(part));
	}
	if (parts.empty()) {
		return string();
	}
	return "(" + StringUtil::Join(parts, " " + op + " ") + ")";
}

static bool IsDirectColumnRef(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_REF ||
	       expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
}

static bool ExtractColumnTarget(const string &base_name, const Expression &expr, string &target_name) {
	if (IsDirectColumnRef(expr)) {
		target_name = base_name;
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &fn = expr.Cast<BoundFunctionExpression>();
		if (fn.Function().GetName() == "struct_extract" && fn.GetChildren().size() == 2 &&
		    fn.GetChildren()[1]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			string parent;
			if (!ExtractColumnTarget(base_name, *fn.GetChildren()[0], parent)) {
				return false;
			}
			auto child_name =
			    KeywordHelper::WriteQuoted(fn.GetChildren()[1]->Cast<BoundConstantExpression>().GetValue().ToString(), '"');
			target_name = "(" + parent + ")." + child_name;
			return true;
		}
	}
	return false;
}

static string TransformFilterExpr(const string &column_name, const Expression &expr, column_t column_id) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &cmp = expr.Cast<BoundFunctionExpression>();
		auto cmp_type = cmp.GetExpressionType();
		auto &left = BoundComparisonExpression::Left(cmp);
		auto &right = BoundComparisonExpression::Right(cmp);
		string target;
		const Value *constant = nullptr;
		if (ExtractColumnTarget(column_name, left, target) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant = &right.Cast<BoundConstantExpression>().GetValue();
		} else if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		           ExtractColumnTarget(column_name, right, target)) {
			constant = &left.Cast<BoundConstantExpression>().GetValue();
			cmp_type = FlipComparisonExpression(cmp_type);
		} else {
			return string();
		}
		if (IsVirtualColumn(column_id)) {
			return "FALSE";
		}
		auto constant_string = TransformLiteral(*constant);
		auto operator_string = PostgresFilterPushdown::TransformComparison(cmp_type);
		string comparison = StringUtil::Format("%s %s %s", target, operator_string, constant_string);
		if (constant->type().id() == LogicalTypeId::VARCHAR) {
			comparison += " COLLATE \"C\"";
		}
		return comparison;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto op = conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? "AND" : "OR";
		return CreateConjunctionExpr(column_name, conj.GetChildren(), op, column_id);
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		auto &children = op.GetChildren();
		string target;
		if (children.empty() || !ExtractColumnTarget(column_name, *children[0], target)) {
			return string();
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			return target + " IS NULL";
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return target + " IS NOT NULL";
		case ExpressionType::COMPARE_IN: {
			string in_list;
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					return string();
				}
				if (!in_list.empty()) {
					in_list += ", ";
				}
				in_list += TransformLiteral(children[i]->Cast<BoundConstantExpression>().GetValue());
			}
			return target + " IN (" + in_list + ")";
		}
		default:
			return string();
		}
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<OptionalFilterFunctionData>();
			return data.child_filter_expr ? TransformFilterExpr(column_name, *data.child_filter_expr, column_id)
			                              : string();
		}
		if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
			return data.child_filter_expr ? TransformFilterExpr(column_name, *data.child_filter_expr, column_id)
			                              : string();
		}
		if (func.Function().GetName() == DynamicFilterScalarFun::NAME) {
			return string();
		}
		return string();
	}
	default:
		return string();
	}
}

string PostgresFilterPushdown::TransformFilter(string &column_name, TableFilter &filter, column_t column_id) {
	if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
		auto &expr_filter = filter.Cast<ExpressionFilter>();
		if (!expr_filter.expr) {
			return string();
		}
		return TransformFilterExpr(column_name, *expr_filter.expr, column_id);
	}
	return string();
}

string PostgresFilterPushdown::TransformFilters(const vector<column_t> &column_ids,
                                                optional_ptr<TableFilterSet> filters, const vector<string> &names,
                                                const vector<string> &casts) {
	if (!filters || !filters->HasFilters()) {
		// no filters
		return string();
	}
	string result;
	for (auto &entry : *filters) {
		string column_name;
		auto column_id = column_ids[entry.GetIndex().GetIndexUnsafe()];
		if (IsVirtualColumn(column_id)) {
			column_name = "ctid";
		} else {
			column_name = KeywordHelper::WriteQuoted(names[column_id], '"');
			column_name += casts[column_id];
		}
		auto &filter = entry.Filter();
		auto filter_text = TransformFilter(column_name, const_cast<TableFilter &>(filter), column_id);

		if (filter_text.empty()) {
			continue;
		}
		if (!result.empty()) {
			result += " AND ";
		}
		result += filter_text;
	}
	return result;
}

} // namespace duckdb
