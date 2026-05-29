#include "mattsql/planner/default_physical_planner.hpp"

#include "mattsql/common/result_utils.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mattsql {
namespace {

[[nodiscard]] Result<BoundExpressionPtr>
clone_expression(const BoundExpression &expression);

[[nodiscard]] Result<BoundExpressionPtr>
clone_literal(const BoundLiteralExpression &expression) {
  auto clone = std::make_unique<BoundLiteralExpression>();
  clone->kind = BoundExpressionKind::Literal;
  clone->type = expression.type;
  clone->value = expression.value;
  return ok_result<BoundExpressionPtr>(std::move(clone));
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_column(const BoundColumnExpression &expression) {
  auto clone = std::make_unique<BoundColumnExpression>();
  clone->kind = BoundExpressionKind::Column;
  clone->type = expression.type;
  clone->table_id = expression.table_id;
  clone->column_id = expression.column_id;
  clone->table_name = expression.table_name;
  clone->column_name = expression.column_name;
  return ok_result<BoundExpressionPtr>(std::move(clone));
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_unary(const BoundUnaryExpression &expression) {
  if (expression.operand == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                            "unary expression is missing its operand");
  }

  auto operand = clone_expression(*expression.operand);
  if (!status_ok(operand.status)) {
    return operand;
  }

  auto clone = std::make_unique<BoundUnaryExpression>();
  clone->kind = BoundExpressionKind::Unary;
  clone->type = expression.type;
  clone->op = expression.op;
  clone->operand = std::move(*operand.value);
  return ok_result<BoundExpressionPtr>(std::move(clone));
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_binary(const BoundBinaryExpression &expression) {
  if (expression.left == nullptr || expression.right == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                            "binary expression is missing an operand");
  }

  auto left = clone_expression(*expression.left);
  if (!status_ok(left.status)) {
    return left;
  }

  auto right = clone_expression(*expression.right);
  if (!status_ok(right.status)) {
    return right;
  }

  auto clone = std::make_unique<BoundBinaryExpression>();
  clone->kind = BoundExpressionKind::Binary;
  clone->type = expression.type;
  clone->left = std::move(*left.value);
  clone->op = expression.op;
  clone->right = std::move(*right.value);
  return ok_result<BoundExpressionPtr>(std::move(clone));
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_star(const BoundStarExpression &expression) {
  (void)expression;
  return error_result<BoundExpressionPtr>(
      ErrorCode::PlanError,
      "star expressions must be expanded before physical planning");
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_expression(const BoundExpression &expression) {
  if (const auto *literal = dynamic_cast<const BoundLiteralExpression *>(&expression)) {
    return clone_literal(*literal);
  }
  if (const auto *column = dynamic_cast<const BoundColumnExpression *>(&expression)) {
    return clone_column(*column);
  }
  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&expression)) {
    return clone_unary(*unary);
  }
  if (const auto *binary = dynamic_cast<const BoundBinaryExpression *>(&expression)) {
    return clone_binary(*binary);
  }
  if (const auto *star = dynamic_cast<const BoundStarExpression *>(&expression)) {
    return clone_star(*star);
  }

  return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                          "unsupported bound expression");
}

[[nodiscard]] Result<std::vector<BoundExpressionPtr>>
clone_expressions(const std::vector<BoundExpressionPtr> &expressions) {
  std::vector<BoundExpressionPtr> clones;
  clones.reserve(expressions.size());

  for (const auto &expression : expressions) {
    if (expression == nullptr) {
      return error_result<std::vector<BoundExpressionPtr>>(
          ErrorCode::PlanError, "expression list contains a null expression");
    }

    auto clone = clone_expression(*expression);
    if (!status_ok(clone.status)) {
      return error_result<std::vector<BoundExpressionPtr>>(std::move(clone.status));
    }
    clones.push_back(std::move(*clone.value));
  }

  return ok_result(std::move(clones));
}

[[nodiscard]] Result<std::vector<std::vector<BoundExpressionPtr>>>
clone_expression_rows(const std::vector<std::vector<BoundExpressionPtr>> &rows) {
  std::vector<std::vector<BoundExpressionPtr>> clones;
  clones.reserve(rows.size());

  for (const auto &row : rows) {
    auto clone = clone_expressions(row);
    if (!status_ok(clone.status)) {
      return error_result<std::vector<std::vector<BoundExpressionPtr>>>(
          std::move(clone.status));
    }
    clones.push_back(std::move(*clone.value));
  }

  return ok_result(std::move(clones));
}

[[nodiscard]] TableStorageReference
make_table_storage_reference(const TableInfo &table) {
  TableStorageReference storage;
  storage.method = TableStorageMethod::Heap;
  storage.table_id = table.id;
  storage.table_name = table.name;
  storage.root_page_id = table.heap_root_page_id;
  storage.schema = table.schema;
  return storage;
}

[[nodiscard]] Result<PhysicalPlanPtr> plan_node(const LogicalPlan &plan);

[[nodiscard]] Result<std::vector<PhysicalPlanPtr>>
plan_children(const std::vector<LogicalPlanPtr> &children) {
  std::vector<PhysicalPlanPtr> planned_children;
  planned_children.reserve(children.size());

  for (const auto &child : children) {
    if (child == nullptr) {
      return error_result<std::vector<PhysicalPlanPtr>>(
          ErrorCode::PlanError, "logical plan contains a null child");
    }

    auto planned = plan_node(*child);
    if (!status_ok(planned.status)) {
      return error_result<std::vector<PhysicalPlanPtr>>(std::move(planned.status));
    }
    planned_children.push_back(std::move(*planned.value));
  }

  return ok_result(std::move(planned_children));
}

[[nodiscard]] Status require_child_count(const LogicalPlan &plan, std::size_t expected,
                                         std::string_view operator_name) {
  if (plan.children.size() != expected) {
    return error_status(ErrorCode::PlanError,
                        std::string(operator_name) + " has the wrong number of inputs");
  }

  return ok_status();
}

[[nodiscard]] Result<PhysicalPlanPtr>
plan_create_table(const LogicalCreateTable &logical) {
  const auto child_status = require_child_count(logical, 0, "CREATE TABLE");
  if (!status_ok(child_status)) {
    return error_result<PhysicalPlanPtr>(child_status);
  }
  if (logical.request.name.empty()) {
    return error_result<PhysicalPlanPtr>(ErrorCode::PlanError,
                                         "CREATE TABLE requires a table name");
  }
  if (logical.request.schema.columns.empty()) {
    return error_result<PhysicalPlanPtr>(ErrorCode::PlanError,
                                         "CREATE TABLE requires at least one column");
  }

  auto physical = std::make_unique<PhysicalCreateTable>();
  physical->kind = PhysicalOperatorKind::CreateTable;
  physical->request = logical.request;
  physical->storage_method = TableStorageMethod::Heap;
  return ok_result<PhysicalPlanPtr>(std::move(physical));
}

[[nodiscard]] Result<PhysicalPlanPtr> plan_values(const LogicalValues &logical) {
  const auto child_status = require_child_count(logical, 0, "VALUES");
  if (!status_ok(child_status)) {
    return error_result<PhysicalPlanPtr>(child_status);
  }

  auto rows = clone_expression_rows(logical.rows);
  if (!status_ok(rows.status)) {
    return error_result<PhysicalPlanPtr>(std::move(rows.status));
  }

  auto physical = std::make_unique<PhysicalValues>();
  physical->kind = PhysicalOperatorKind::Values;
  physical->rows = std::move(*rows.value);
  physical->tuples = logical.tuples;
  return ok_result<PhysicalPlanPtr>(std::move(physical));
}

[[nodiscard]] Result<PhysicalPlanPtr> plan_seq_scan(const LogicalSeqScan &logical) {
  const auto child_status = require_child_count(logical, 0, "SeqScan");
  if (!status_ok(child_status)) {
    return error_result<PhysicalPlanPtr>(child_status);
  }
  if (logical.table.name.empty()) {
    return error_result<PhysicalPlanPtr>(ErrorCode::PlanError,
                                         "SeqScan requires a table");
  }

  auto physical = std::make_unique<PhysicalSeqScan>();
  physical->kind = PhysicalOperatorKind::SeqScan;
  physical->table = logical.table;
  physical->storage = make_table_storage_reference(logical.table);
  return ok_result<PhysicalPlanPtr>(std::move(physical));
}

[[nodiscard]] Result<PhysicalPlanPtr> plan_filter(const LogicalFilter &logical) {
  const auto child_status = require_child_count(logical, 1, "Filter");
  if (!status_ok(child_status)) {
    return error_result<PhysicalPlanPtr>(child_status);
  }
  if (logical.predicate == nullptr) {
    return error_result<PhysicalPlanPtr>(ErrorCode::PlanError,
                                         "filter requires a predicate");
  }
  if (logical.predicate->type != SqlType::Boolean) {
    return error_result<PhysicalPlanPtr>(ErrorCode::TypeMismatch,
                                         "filter predicate must be BOOLEAN");
  }

  auto children = plan_children(logical.children);
  if (!status_ok(children.status)) {
    return error_result<PhysicalPlanPtr>(std::move(children.status));
  }

  auto predicate = clone_expression(*logical.predicate);
  if (!status_ok(predicate.status)) {
    return error_result<PhysicalPlanPtr>(std::move(predicate.status));
  }

  auto physical = std::make_unique<PhysicalFilter>();
  physical->kind = PhysicalOperatorKind::Filter;
  physical->predicate = std::move(*predicate.value);
  physical->children = std::move(*children.value);
  return ok_result<PhysicalPlanPtr>(std::move(physical));
}

[[nodiscard]] Result<PhysicalPlanPtr>
plan_projection(const LogicalProjection &logical) {
  const auto child_status = require_child_count(logical, 1, "Projection");
  if (!status_ok(child_status)) {
    return error_result<PhysicalPlanPtr>(child_status);
  }
  if (logical.projections.empty()) {
    return error_result<PhysicalPlanPtr>(ErrorCode::PlanError,
                                         "projection requires expressions");
  }

  auto children = plan_children(logical.children);
  if (!status_ok(children.status)) {
    return error_result<PhysicalPlanPtr>(std::move(children.status));
  }

  auto projections = clone_expressions(logical.projections);
  if (!status_ok(projections.status)) {
    return error_result<PhysicalPlanPtr>(std::move(projections.status));
  }

  auto physical = std::make_unique<PhysicalProjection>();
  physical->kind = PhysicalOperatorKind::Projection;
  physical->projections = std::move(*projections.value);
  physical->children = std::move(*children.value);
  return ok_result<PhysicalPlanPtr>(std::move(physical));
}

[[nodiscard]] Result<PhysicalPlanPtr> plan_insert(const LogicalInsert &logical) {
  const auto child_status = require_child_count(logical, 1, "Insert");
  if (!status_ok(child_status)) {
    return error_result<PhysicalPlanPtr>(child_status);
  }
  if (logical.table.name.empty()) {
    return error_result<PhysicalPlanPtr>(ErrorCode::PlanError,
                                         "INSERT requires a target table");
  }

  auto children = plan_children(logical.children);
  if (!status_ok(children.status)) {
    return error_result<PhysicalPlanPtr>(std::move(children.status));
  }

  auto physical = std::make_unique<PhysicalInsert>();
  physical->kind = PhysicalOperatorKind::Insert;
  physical->table = logical.table;
  physical->storage = make_table_storage_reference(logical.table);
  physical->children = std::move(*children.value);
  return ok_result<PhysicalPlanPtr>(std::move(physical));
}

[[nodiscard]] Result<PhysicalPlanPtr> plan_node(const LogicalPlan &plan) {
  if (const auto *create = dynamic_cast<const LogicalCreateTable *>(&plan)) {
    return plan_create_table(*create);
  }
  if (const auto *insert = dynamic_cast<const LogicalInsert *>(&plan)) {
    return plan_insert(*insert);
  }
  if (const auto *projection = dynamic_cast<const LogicalProjection *>(&plan)) {
    return plan_projection(*projection);
  }
  if (const auto *filter = dynamic_cast<const LogicalFilter *>(&plan)) {
    return plan_filter(*filter);
  }
  if (const auto *values = dynamic_cast<const LogicalValues *>(&plan)) {
    return plan_values(*values);
  }
  if (const auto *scan = dynamic_cast<const LogicalSeqScan *>(&plan)) {
    return plan_seq_scan(*scan);
  }

  return error_result<PhysicalPlanPtr>(ErrorCode::PlanError,
                                       "unsupported logical plan node");
}

} // namespace

Result<PhysicalPlanPtr> DefaultPhysicalPlanner::Plan(const LogicalPlan &plan) {
  return plan_node(plan);
}

} // namespace mattsql
