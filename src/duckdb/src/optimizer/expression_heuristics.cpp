#include "duckdb/optimizer/expression_heuristics.hpp"

#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/struct_filter.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> ExpressionHeuristics::Rewrite(unique_ptr<LogicalOperator> op) {
	VisitOperator(*op);
	return op;
}

void ExpressionHeuristics::VisitOperator(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		// reorder all filter expressions
		if (op.expressions.size() > 1) {
			ReorderExpressions(op.expressions);
		}
	}

	// traverse recursively through the operator tree
	VisitOperatorChildren(op);
	VisitOperatorExpressions(op);
}

unique_ptr<Expression> ExpressionHeuristics::VisitReplace(BoundConjunctionExpression &expr,
                                                          unique_ptr<Expression> *expr_ptr) {
	ReorderExpressions(expr.children);
	return nullptr;
}

void ExpressionHeuristics::ReorderExpressions(vector<unique_ptr<Expression>> &expressions) {
	struct ExpressionCosts {
		unique_ptr<Expression> expr;
		idx_t cost;

		bool operator==(const ExpressionCosts &p) const {
			return cost == p.cost;
		}
		bool operator<(const ExpressionCosts &p) const {
			return cost < p.cost;
		}
	};

	for (idx_t i = 0; i < expressions.size(); i++) {
		if (expressions[i]->CanThrow()) {
			// do not allow reordering if an expression can throw
			return;
		}
	}

	vector<ExpressionCosts> expression_costs;
	expression_costs.reserve(expressions.size());
	// iterate expressions, get cost for each one
	for (idx_t i = 0; i < expressions.size(); i++) {
		idx_t cost = Cost(*expressions[i]);
		expression_costs.push_back({std::move(expressions[i]), cost});
	}

	// sort by cost and put back in place
	sort(expression_costs.begin(), expression_costs.end());
	for (idx_t i = 0; i < expression_costs.size(); i++) {
		expressions[i] = std::move(expression_costs[i].expr);
	}
}

idx_t ExpressionHeuristics::ExpressionCost(BoundBetweenExpression &expr) {
	return Cost(*expr.input) + Cost(*expr.lower) + Cost(*expr.upper) + 10;
}

idx_t ExpressionHeuristics::ExpressionCost(BoundCaseExpression &expr) {
	// CASE WHEN check THEN result_if_true ELSE result_if_false END
	idx_t case_cost = 0;
	for (auto &case_check : expr.case_checks) {
		case_cost += Cost(*case_check.then_expr);
		case_cost += Cost(*case_check.when_expr);
	}
	case_cost += Cost(*expr.else_expr);
	return case_cost;
}

idx_t ExpressionHeuristics::ExpressionCost(BoundCastExpression &expr) {
	// OPERATOR_CAST
	// determine cast cost by comparing cast_expr.source_type and cast_expr_target_type
	idx_t cast_cost = 0;
	if (expr.return_type != expr.source_type()) {
		// if cast from or to varchar
		// TODO: we might want to add more cases
		if (expr.return_type.id() == LogicalTypeId::VARCHAR || expr.source_type().id() == LogicalTypeId::VARCHAR ||
		    expr.return_type.id() == LogicalTypeId::BLOB || expr.source_type().id() == LogicalTypeId::BLOB) {
			cast_cost = 200;
		} else {
			cast_cost = 5;
		}
	}
	return Cost(*expr.child) + cast_cost;
}

idx_t ExpressionHeuristics::ExpressionCost(BoundComparisonExpression &expr) {
	// COMPARE_EQUAL, COMPARE_NOTEQUAL, COMPARE_GREATERTHAN, COMPARE_GREATERTHANOREQUALTO, COMPARE_LESSTHAN,
	// COMPARE_LESSTHANOREQUALTO
	return Cost(*expr.left) + 5 + Cost(*expr.right);
}

idx_t ExpressionHeuristics::ExpressionCost(BoundConjunctionExpression &expr) {
	// CONJUNCTION_AND, CONJUNCTION_OR
	idx_t cost = 5;
	for (auto &child : expr.children) {
		cost += Cost(*child);
	}
	return cost;
}

idx_t ExpressionHeuristics::ExpressionCost(BoundFunctionExpression &expr) {
	unordered_map<std::string, idx_t> function_costs = {
	    {"+", 5},       {"-", 5},    {"&", 5},          {"#", 5},
	    {">>", 5},      {"<<", 5},   {"abs", 5},        {"*", 10},
	    {"%", 10},      {"/", 15},   {"date_part", 20}, {"year", 20},
	    {"round", 100}, {"~~", 200}, {"!~~", 200},      {"regexp_matches", 200},
	    {"||", 200}};

	idx_t cost_children = 0;
	for (auto &child : expr.children) {
		cost_children += Cost(*child);
	}

	auto cost_function = function_costs.find(expr.function.name);
	if (cost_function != function_costs.end()) {
		return cost_children + cost_function->second;
	} else {
		return cost_children + 1000;
	}
}

idx_t ExpressionHeuristics::ExpressionCost(BoundOperatorExpression &expr, ExpressionType expr_type) {
	idx_t sum = 0;
	for (auto &child : expr.children) {
		sum += Cost(*child);
	}

	// OPERATOR_IS_NULL, OPERATOR_IS_NOT_NULL
	if (expr_type == ExpressionType::OPERATOR_IS_NULL || expr_type == ExpressionType::OPERATOR_IS_NOT_NULL) {
		return sum + 5;
	} else if (expr_type == ExpressionType::COMPARE_IN || expr_type == ExpressionType::COMPARE_NOT_IN) {
		// COMPARE_IN, COMPARE_NOT_IN
		return sum + (expr.children.size() - 1) * 100;
	} else if (expr_type == ExpressionType::OPERATOR_NOT) {
		// OPERATOR_NOT
		return sum + 10; // TODO: evaluate via measured runtimes
	} else {
		return sum + 1000;
	}
}

idx_t ExpressionHeuristics::ExpressionCost(PhysicalType return_type, idx_t multiplier) {
	// TODO: ajust values according to benchmark results
	switch (return_type) {
	case PhysicalType::VARCHAR:
		return 5 * multiplier;
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return 2 * multiplier;
	default:
		return 1 * multiplier;
	}
}

idx_t ExpressionHeuristics::Cost(Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CASE: {
		auto &case_expr = expr.Cast<BoundCaseExpression>();
		return ExpressionCost(case_expr);
	}
	case ExpressionClass::BOUND_BETWEEN: {
		auto &between_expr = expr.Cast<BoundBetweenExpression>();
		return ExpressionCost(between_expr);
	}
	case ExpressionClass::BOUND_CAST: {
		auto &cast_expr = expr.Cast<BoundCastExpression>();
		return ExpressionCost(cast_expr);
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &comp_expr = expr.Cast<BoundComparisonExpression>();
		return ExpressionCost(comp_expr);
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj_expr = expr.Cast<BoundConjunctionExpression>();
		return ExpressionCost(conj_expr);
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func_expr = expr.Cast<BoundFunctionExpression>();
		return ExpressionCost(func_expr);
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		return ExpressionCost(op_expr, expr.GetExpressionType());
	}
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &col_expr = expr.Cast<BoundColumnRefExpression>();
		return ExpressionCost(col_expr.return_type.InternalType(), 8);
	}
	case ExpressionClass::BOUND_CONSTANT: {
		auto &const_expr = expr.Cast<BoundConstantExpression>();
		return ExpressionCost(const_expr.return_type.InternalType(), 1);
	}
	case ExpressionClass::BOUND_PARAMETER: {
		auto &const_expr = expr.Cast<BoundParameterExpression>();
		return ExpressionCost(const_expr.return_type.InternalType(), 1);
	}
	case ExpressionClass::BOUND_REF: {
		auto &col_expr = expr.Cast<BoundColumnRefExpression>();
		return ExpressionCost(col_expr.return_type.InternalType(), 8);
	}
	default: {
		break;
	}
	}

	// return a very high value if nothing matches
	return 1000;
}

idx_t ExpressionHeuristics::Cost(TableFilter &filter) {
	switch (filter.filter_type) {
	case TableFilterType::DYNAMIC_FILTER:
	case TableFilterType::OPTIONAL_FILTER:
		return 0;
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction_and = filter.Cast<ConjunctionOrFilter>();
		idx_t cost = 5;
		for (auto &child_filter : conjunction_and.child_filters) {
			cost += Cost(*child_filter);
		}
		return cost;
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction_and = filter.Cast<ConjunctionAndFilter>();
		idx_t cost = 5;
		for (auto &child_filter : conjunction_and.child_filters) {
			cost += Cost(*child_filter);
		}
		return cost;
	}
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = filter.Cast<ConstantFilter>();
		return ExpressionCost(constant_filter.constant.type().InternalType(), 1);
	}
	case TableFilterType::IS_NULL:
	case TableFilterType::IS_NOT_NULL:
		return 5;
	case TableFilterType::STRUCT_EXTRACT: {
		auto &struct_filter = filter.Cast<StructFilter>();
		return Cost(*struct_filter.child_filter);
	}
	default:
		return 1000;
	}
}

vector<idx_t> ExpressionHeuristics::GetInitialOrder(const TableFilterSet &table_filters) {
	struct FilterCost {
		idx_t index;
		idx_t cost;

		bool operator==(const FilterCost &p) const {
			return cost == p.cost;
		}
		bool operator<(const FilterCost &p) const {
			return cost < p.cost;
		}
	};
	vector<FilterCost> filter_costs;
	idx_t filter_index = 0;
	for (auto &entry : table_filters.filters) {
		FilterCost cost;
		cost.index = filter_index;
		cost.cost = Cost(*entry.second);
		filter_costs.push_back(cost);
		filter_index++;
	}
	// sort by cost and put back in place
	sort(filter_costs.begin(), filter_costs.end());
	vector<idx_t> initial_permutation;
	for (idx_t i = 0; i < filter_costs.size(); i++) {
		initial_permutation.push_back(filter_costs[i].index);
	}
	return initial_permutation;
}

} // namespace duckdb
