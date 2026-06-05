#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/optimizer/default_optimizer.hpp"

#include "sql_pipeline_test_utils.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using test::as;
using test::logical_plan_sql;
using test::make_catalog;
using test::optimized_logical_plan_sql;

class RecordingRule final : public mattsql::OptimizerRule {
public:
  RecordingRule(std::string name, std::vector<std::string> &order)
      : name_(std::move(name)), order_(&order) {}

  std::string_view Name() const override { return name_; }

  mattsql::Result<mattsql::LogicalPlanPtr>
  Apply(mattsql::LogicalPlanPtr plan) override {
    order_->push_back(name_);
    return mattsql::ok_result(std::move(plan));
  }

private:
  std::string name_;
  std::vector<std::string> *order_;
};

class FailingRule final : public mattsql::OptimizerRule {
public:
  std::string_view Name() const override { return "failing_rule"; }

  mattsql::Result<mattsql::LogicalPlanPtr>
  Apply(mattsql::LogicalPlanPtr plan) override {
    (void)plan;
    return mattsql::error_result<mattsql::LogicalPlanPtr>(mattsql::ErrorCode::Internal,
                                                          "forced optimizer failure");
  }
};

class EmptyNameRule final : public mattsql::OptimizerRule {
public:
  std::string_view Name() const override { return {}; }

  mattsql::Result<mattsql::LogicalPlanPtr>
  Apply(mattsql::LogicalPlanPtr plan) override {
    return mattsql::ok_result(std::move(plan));
  }
};

class NullSuccessRule final : public mattsql::OptimizerRule {
public:
  std::string_view Name() const override { return "null_success_rule"; }

  mattsql::Result<mattsql::LogicalPlanPtr>
  Apply(mattsql::LogicalPlanPtr plan) override {
    (void)plan;
    mattsql::Result<mattsql::LogicalPlanPtr> result;
    result.value.emplace(nullptr);
    return result;
  }
};

class LyingLogicalProjection final : public mattsql::LogicalPlan {
public:
  [[nodiscard]] mattsql::LogicalOperatorKind Kind() const override {
    return mattsql::LogicalOperatorKind::Projection;
  }

  std::vector<mattsql::BoundExpressionPtr> projections;
  std::vector<std::string> projection_names;
};

} // namespace

/// Verifies literal-only projection expressions are folded recursively.
TEST_CASE(optimizer_folds_constant_projection_expressions) {
  mattsql::InMemoryCatalog catalog;

  const auto optimized = optimized_logical_plan_sql("SELECT 1 + 2 * 3;", catalog);

  EXPECT_TRUE(mattsql::status_ok(optimized.status));
  const auto *projection = as<mattsql::LogicalProjection>(optimized.value->get());
  EXPECT_EQ(projection->projections.size(), 1U);

  const auto *literal =
      as<mattsql::BoundLiteralExpression>(projection->projections[0].get());
  EXPECT_TRUE(literal->type == mattsql::SqlType::Integer);
  EXPECT_EQ(std::get<std::int64_t>(literal->value), std::int64_t{7});
}

/// Verifies compile-time TRUE filters are removed from table scan plans.
TEST_CASE(optimizer_removes_true_constant_filter) {
  auto catalog = make_catalog();

  const auto optimized =
      optimized_logical_plan_sql("SELECT id FROM users WHERE 1 = 1;", catalog);

  EXPECT_TRUE(mattsql::status_ok(optimized.status));
  const auto *projection = as<mattsql::LogicalProjection>(optimized.value->get());
  EXPECT_EQ(projection->children.size(), 1U);
  const auto *scan = as<mattsql::LogicalSeqScan>(projection->children[0].get());
  EXPECT_EQ(scan->table.name, std::string("users"));
}

/// Verifies compile-time FALSE filters become an empty logical VALUES input.
TEST_CASE(optimizer_replaces_false_constant_filter_with_empty_values) {
  auto catalog = make_catalog();

  const auto optimized =
      optimized_logical_plan_sql("SELECT id FROM users WHERE 1 = 0;", catalog);

  EXPECT_TRUE(mattsql::status_ok(optimized.status));
  const auto *projection = as<mattsql::LogicalProjection>(optimized.value->get());
  EXPECT_EQ(projection->children.size(), 1U);
  const auto *values = as<mattsql::LogicalValues>(projection->children[0].get());
  EXPECT_EQ(values->rows.size(), 0U);
}

/// Verifies INSERT VALUES payload expressions are optimized before execution.
TEST_CASE(optimizer_folds_insert_values_payloads) {
  auto catalog = make_catalog();

  const auto optimized = optimized_logical_plan_sql(
      "INSERT INTO users VALUES (1 + 2, 'Ada', 1);", catalog);

  EXPECT_TRUE(mattsql::status_ok(optimized.status));
  const auto *insert = as<mattsql::LogicalInsert>(optimized.value->get());
  const auto *values = as<mattsql::LogicalValues>(insert->children[0].get());
  EXPECT_EQ(values->rows.size(), 1U);
  EXPECT_EQ(values->rows[0].size(), 3U);

  const auto *id = as<mattsql::BoundLiteralExpression>(values->rows[0][0].get());
  EXPECT_EQ(std::get<std::int64_t>(id->value), std::int64_t{3});
}

/// Verifies unsafe constant folds such as division by zero are left intact.
TEST_CASE(optimizer_leaves_division_by_zero_for_execution) {
  mattsql::InMemoryCatalog catalog;

  const auto optimized = optimized_logical_plan_sql("SELECT 1 / 0;", catalog);

  EXPECT_TRUE(mattsql::status_ok(optimized.status));
  const auto *projection = as<mattsql::LogicalProjection>(optimized.value->get());
  const auto *divide =
      as<mattsql::BoundBinaryExpression>(projection->projections[0].get());
  EXPECT_TRUE(divide->op == mattsql::BinaryOperator::Divide);
}

/// Verifies custom rules run after built-ins and preserve insertion order.
TEST_CASE(optimizer_runs_custom_rules_in_order) {
  mattsql::InMemoryCatalog catalog;
  auto plan = logical_plan_sql("CREATE TABLE projects (id INT);", catalog);
  EXPECT_TRUE(mattsql::status_ok(plan.status));

  std::vector<std::string> order;
  mattsql::DefaultOptimizer optimizer;
  EXPECT_TRUE(mattsql::status_ok(
      optimizer.AddRule(std::make_unique<RecordingRule>("first_custom", order))));
  EXPECT_TRUE(mattsql::status_ok(
      optimizer.AddRule(std::make_unique<RecordingRule>("second_custom", order))));

  const auto optimized = optimizer.Optimize(std::move(*plan.value));

  EXPECT_TRUE(mattsql::status_ok(optimized.status));
  EXPECT_EQ(order.size(), 2U);
  EXPECT_EQ(order[0], std::string("first_custom"));
  EXPECT_EQ(order[1], std::string("second_custom"));
}

/// Verifies invalid optimizer inputs and duplicate rules are rejected.
TEST_CASE(optimizer_rejects_invalid_inputs) {
  mattsql::DefaultOptimizer optimizer;

  EXPECT_TRUE(optimizer.AddRule(nullptr).code == mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(optimizer.AddRule(std::make_unique<EmptyNameRule>()).code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(
      optimizer.AddRule(std::make_unique<mattsql::ConstantFoldingRule>()).code ==
      mattsql::ErrorCode::AlreadyExists);
  EXPECT_TRUE(optimizer.Optimize(nullptr).status.code ==
              mattsql::ErrorCode::InvalidArgument);
}

/// Verifies rule errors and invalid rule return values stop optimization.
TEST_CASE(optimizer_propagates_rule_failures) {
  {
    mattsql::InMemoryCatalog catalog;
    auto plan = logical_plan_sql("CREATE TABLE projects (id INT);", catalog);
    EXPECT_TRUE(mattsql::status_ok(plan.status));

    mattsql::DefaultOptimizer optimizer;
    EXPECT_TRUE(mattsql::status_ok(optimizer.AddRule(std::make_unique<FailingRule>())));

    const auto optimized = optimizer.Optimize(std::move(*plan.value));
    EXPECT_TRUE(optimized.status.code == mattsql::ErrorCode::Internal);
  }

  {
    mattsql::InMemoryCatalog catalog;
    auto plan = logical_plan_sql("CREATE TABLE projects (id INT);", catalog);
    EXPECT_TRUE(mattsql::status_ok(plan.status));

    mattsql::DefaultOptimizer optimizer;
    EXPECT_TRUE(
        mattsql::status_ok(optimizer.AddRule(std::make_unique<NullSuccessRule>())));

    const auto optimized = optimizer.Optimize(std::move(*plan.value));
    EXPECT_TRUE(optimized.status.code == mattsql::ErrorCode::Internal);
  }
}

/// Verifies malformed logical plans are rejected by the built-in rules.
TEST_CASE(optimizer_rejects_malformed_logical_plans) {
  {
    auto projection = std::make_unique<mattsql::LogicalProjection>();
    projection->projections.push_back(nullptr);

    mattsql::DefaultOptimizer optimizer;
    const auto optimized = optimizer.Optimize(std::move(projection));
    EXPECT_TRUE(optimized.status.code == mattsql::ErrorCode::PlanError);
  }

  {
    auto filter = std::make_unique<mattsql::LogicalFilter>();
    filter->children.push_back(std::make_unique<mattsql::LogicalValues>());

    mattsql::DefaultOptimizer optimizer;
    const auto optimized = optimizer.Optimize(std::move(filter));
    EXPECT_TRUE(optimized.status.code == mattsql::ErrorCode::PlanError);
  }

  {
    auto projection = std::make_unique<mattsql::LogicalProjection>();
    auto star = std::make_unique<mattsql::BoundStarExpression>();
    projection->projections.push_back(std::move(star));

    mattsql::DefaultOptimizer optimizer;
    const auto optimized = optimizer.Optimize(std::move(projection));
    EXPECT_TRUE(optimized.status.code == mattsql::ErrorCode::PlanError);
  }
}

/// Verifies optimizer dispatch checks runtime type, not just reported Kind.
TEST_CASE(optimizer_rejects_misreported_logical_plan_kind) {
  auto projection = std::make_unique<LyingLogicalProjection>();
  projection->projections.push_back(test::make_integer_literal(1));

  mattsql::DefaultOptimizer optimizer;
  const auto optimized = optimizer.Optimize(std::move(projection));

  EXPECT_TRUE(optimized.status.code == mattsql::ErrorCode::PlanError);
}
