#pragma once

#include "mattsql/optimizer/optimizer.hpp"

#include <memory>
#include <vector>

namespace mattsql {

class ConstantFoldingRule final : public OptimizerRule {
public:
  /// Stable name for the recursive constant-expression rewrite.
  std::string_view Name() const override;

  /// Folds literal-only scalar expressions inside the logical plan tree.
  Result<LogicalPlanPtr> Apply(LogicalPlanPtr plan) override;
};

class RemoveConstantFilterRule final : public OptimizerRule {
public:
  /// Stable name for the constant filter pruning rewrite.
  std::string_view Name() const override;

  /// Removes filters whose predicates are compile-time TRUE and replaces
  /// compile-time FALSE filters with an empty VALUES input.
  Result<LogicalPlanPtr> Apply(LogicalPlanPtr plan) override;
};

class DefaultOptimizer final : public Optimizer {
public:
  /// Creates an optimizer with the standard logical rewrite pipeline.
  DefaultOptimizer();

  /// Adds a rule after the built-in logical rewrites.
  Status AddRule(std::unique_ptr<OptimizerRule> rule) override;

  /// Applies each rule to the logical plan root in pipeline order.
  Result<LogicalPlanPtr> Optimize(LogicalPlanPtr plan) override;

private:
  std::vector<std::unique_ptr<OptimizerRule>> rules_;
};

} // namespace mattsql
