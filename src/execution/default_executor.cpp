#include "mattsql/execution/default_executor.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/value_utils.hpp"
#include "mattsql/execution/expressions/evaluator.hpp"
#include "mattsql/planner/plan_utils.hpp"

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mattsql {
namespace {

struct ExecutionResult {
  std::vector<std::string> columns;
  TableSchema schema;
  std::vector<std::vector<Value>> rows;
};

[[nodiscard]] QueryResult to_query_result(ExecutionResult result) {
  QueryResult query_result;
  query_result.columns = std::move(result.columns);
  query_result.rows = std::move(result.rows);
  return query_result;
}

[[nodiscard]] Status require_active(const Transaction &transaction) {
  if (transaction.State() != TransactionState::Active) {
    return error_status(ErrorCode::TransactionConflict, "transaction is not active");
  }

  return ok_status();
}

[[nodiscard]] Status require_writable(const Transaction &transaction) {
  const auto active_status = require_active(transaction);
  if (!status_ok(active_status)) {
    return active_status;
  }
  if (transaction.Mode() != TransactionMode::ReadWrite) {
    return error_status(ErrorCode::TransactionConflict,
                        "statement requires a read-write transaction");
  }

  return ok_status();
}

class ExecutionDriver {
public:
  ExecutionDriver(Catalog &catalog, TableStorageManager &storage,
                  const TupleCodec &tuple_codec, Transaction &transaction)
      : catalog_(catalog), storage_(storage), tuple_codec_(tuple_codec),
        transaction_(transaction) {}

  Result<QueryResult> Execute(const PhysicalPlan &plan) {
    auto result = execute_plan(plan);
    if (!result.ok()) {
      return error_result<QueryResult>(std::move(result.status));
    }

    return ok_result(to_query_result(std::move(result).TakeValue()));
  }

private:
  Result<ExecutionResult> execute_plan(const PhysicalPlan &plan) {
    if (const auto *create = dynamic_cast<const PhysicalCreateTable *>(&plan)) {
      return execute_create_table(*create);
    }
    if (const auto *insert = dynamic_cast<const PhysicalInsert *>(&plan)) {
      return execute_insert(*insert);
    }
    if (const auto *projection = dynamic_cast<const PhysicalProjection *>(&plan)) {
      return execute_projection(*projection);
    }
    if (const auto *filter = dynamic_cast<const PhysicalFilter *>(&plan)) {
      return execute_filter(*filter);
    }
    if (const auto *values = dynamic_cast<const PhysicalValues *>(&plan)) {
      return execute_values(*values);
    }
    if (const auto *scan = dynamic_cast<const PhysicalSeqScan *>(&plan)) {
      return execute_seq_scan(*scan);
    }

    return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                         "unsupported physical plan node");
  }

  Result<ExecutionResult> execute_create_table(const PhysicalCreateTable &plan) {
    const auto child_status =
        RequireLeaf(plan, "CREATE TABLE", ErrorCode::ExecutionError);
    if (!status_ok(child_status)) {
      return error_result<ExecutionResult>(child_status);
    }
    const auto writable_status = require_writable(transaction_);
    if (!status_ok(writable_status)) {
      return error_result<ExecutionResult>(writable_status);
    }
    if (plan.storage_method != TableStorageMethod::Heap) {
      return error_result<ExecutionResult>(ErrorCode::NotSupported,
                                           "only heap table storage is supported");
    }

    auto table = catalog_.CreateTable(plan.request);
    if (!table.ok()) {
      return error_result<ExecutionResult>(std::move(table.status));
    }

    auto heap_root = storage_.CreateHeap(transaction_, table.Value());
    if (!heap_root.ok()) {
      (void)catalog_.DropTable(table.Value().name);
      return error_result<ExecutionResult>(std::move(heap_root.status));
    }

    const auto root_status =
        catalog_.SetTableHeapRoot(table.Value().id, heap_root.Value());
    if (!status_ok(root_status)) {
      (void)catalog_.DropTable(table.Value().name);
      return error_result<ExecutionResult>(root_status);
    }

    return ok_result(ExecutionResult{});
  }

  Result<ExecutionResult> execute_insert(const PhysicalInsert &plan) {
    const auto child_status =
        RequireChildCount(plan, 1, "INSERT", ErrorCode::ExecutionError);
    if (!status_ok(child_status)) {
      return error_result<ExecutionResult>(child_status);
    }
    const auto writable_status = require_writable(transaction_);
    if (!status_ok(writable_status)) {
      return error_result<ExecutionResult>(writable_status);
    }
    if (plan.storage.method != TableStorageMethod::Heap) {
      return error_result<ExecutionResult>(ErrorCode::NotSupported,
                                           "only heap table storage is supported");
    }

    auto input = execute_plan(*plan.children[0]);
    if (!input.ok()) {
      return input;
    }

    auto heap = storage_.OpenHeap(plan.storage);
    if (!heap.ok()) {
      return error_result<ExecutionResult>(std::move(heap.status));
    }

    for (const auto &row : input.Value().rows) {
      if (row.size() != plan.table.schema.columns.size()) {
        return error_result<ExecutionResult>(
            ErrorCode::ExecutionError, "INSERT row width does not match table schema");
      }

      auto tuple = tuple_codec_.Encode(plan.table.schema, row);
      if (!tuple.ok()) {
        return error_result<ExecutionResult>(std::move(tuple.status));
      }

      ConstBufferView record{std::span<const std::byte>(tuple.Value().bytes.data(),
                                                        tuple.Value().bytes.size())};
      auto inserted = heap.Value()->Insert(transaction_, record);
      if (!inserted.ok()) {
        return error_result<ExecutionResult>(std::move(inserted.status));
      }
    }

    return ok_result(ExecutionResult{});
  }

  Result<ExecutionResult> execute_projection(const PhysicalProjection &plan) {
    const auto child_status =
        RequireChildCount(plan, 1, "projection", ErrorCode::ExecutionError);
    if (!status_ok(child_status)) {
      return error_result<ExecutionResult>(child_status);
    }
    if (plan.projections.empty()) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "projection requires expressions");
    }
    if (!plan.projection_names.empty() &&
        plan.projection_names.size() != plan.projections.size()) {
      return error_result<ExecutionResult>(
          ErrorCode::ExecutionError,
          "projection name count does not match expressions");
    }

    auto input = execute_plan(*plan.children[0]);
    if (!input.ok()) {
      return input;
    }

    ExecutionResult output;
    output.columns.reserve(plan.projections.size());
    output.schema.columns.reserve(plan.projections.size());
    for (std::size_t index = 0; index < plan.projections.size(); ++index) {
      const auto &projection = plan.projections[index];
      if (projection == nullptr) {
        return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                             "projection expression cannot be null");
      }

      ColumnSchema column;
      if (!plan.projection_names.empty() && !plan.projection_names[index].empty()) {
        column.name = plan.projection_names[index];
      } else {
        column.name = ProjectionName(*projection, index);
      }
      column.type = projection->type;
      column.id = static_cast<ColumnId>(index);
      output.columns.push_back(column.name);
      output.schema.columns.push_back(std::move(column));
    }

    output.rows.reserve(input.Value().rows.size());
    for (const auto &row : input.Value().rows) {
      const EvaluationContext context{&input.Value().schema, &row};
      auto projected =
          EvaluateExpressions(expression_evaluator_, plan.projections, context);
      if (!projected.ok()) {
        return error_result<ExecutionResult>(std::move(projected.status));
      }
      output.rows.push_back(std::move(projected).TakeValue());
    }

    return ok_result(std::move(output));
  }

  Result<ExecutionResult> execute_filter(const PhysicalFilter &plan) {
    const auto child_status =
        RequireChildCount(plan, 1, "filter", ErrorCode::ExecutionError);
    if (!status_ok(child_status)) {
      return error_result<ExecutionResult>(child_status);
    }
    if (plan.predicate == nullptr) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "filter requires a predicate");
    }

    auto input = execute_plan(*plan.children[0]);
    if (!input.ok()) {
      return input;
    }

    ExecutionResult output;
    output.columns = input.Value().columns;
    output.schema = input.Value().schema;
    for (const auto &row : input.Value().rows) {
      const EvaluationContext context{&input.Value().schema, &row};
      auto predicate = expression_evaluator_.Evaluate(*plan.predicate, context);
      if (!predicate.ok()) {
        return error_result<ExecutionResult>(std::move(predicate.status));
      }

      auto include = RequireBoolean(predicate.Value(), "filter");
      if (!include.ok()) {
        return error_result<ExecutionResult>(std::move(include.status));
      }
      if (include.Value()) {
        output.rows.push_back(row);
      }
    }

    return ok_result(std::move(output));
  }

  Result<ExecutionResult> execute_values(const PhysicalValues &plan) {
    const auto child_status = RequireLeaf(plan, "VALUES", ErrorCode::ExecutionError);
    if (!status_ok(child_status)) {
      return error_result<ExecutionResult>(child_status);
    }

    ExecutionResult output;
    for (const auto &row : plan.rows) {
      const EvaluationContext context;
      auto values = EvaluateExpressions(expression_evaluator_, row, context);
      if (!values.ok()) {
        return error_result<ExecutionResult>(std::move(values.status));
      }
      output.rows.push_back(std::move(values).TakeValue());
    }

    return ok_result(std::move(output));
  }

  Result<ExecutionResult> execute_seq_scan(const PhysicalSeqScan &plan) {
    const auto child_status = RequireLeaf(plan, "SeqScan", ErrorCode::ExecutionError);
    if (!status_ok(child_status)) {
      return error_result<ExecutionResult>(child_status);
    }
    if (plan.storage.method != TableStorageMethod::Heap) {
      return error_result<ExecutionResult>(ErrorCode::NotSupported,
                                           "only heap table storage is supported");
    }

    auto heap = storage_.OpenHeap(plan.storage);
    if (!heap.ok()) {
      return error_result<ExecutionResult>(std::move(heap.status));
    }

    auto cursor = heap.Value()->Scan(transaction_);
    if (!cursor.ok()) {
      return error_result<ExecutionResult>(std::move(cursor.status));
    }

    ExecutionResult output;
    output.schema = plan.table.schema;
    output.columns.reserve(plan.table.schema.columns.size());
    for (const auto &column : plan.table.schema.columns) {
      output.columns.push_back(column.name);
    }

    while (true) {
      auto record = cursor.Value()->Next();
      if (record.status.code == ErrorCode::NotFound) {
        break;
      }
      if (!record.ok()) {
        return error_result<ExecutionResult>(std::move(record.status));
      }

      auto row = tuple_codec_.Decode(plan.table.schema, record.Value().bytes);
      if (!row.ok()) {
        return error_result<ExecutionResult>(std::move(row.status));
      }
      output.rows.push_back(std::move(row).TakeValue());
    }

    return ok_result(std::move(output));
  }

  Catalog &catalog_;
  TableStorageManager &storage_;
  const TupleCodec &tuple_codec_;
  Transaction &transaction_;
  DefaultExpressionEvaluator expression_evaluator_;
};

} // namespace

DefaultExecutor::DefaultExecutor(Catalog &catalog, TableStorageManager &storage)
    : catalog_(&catalog), storage_(&storage), default_tuple_codec_(),
      tuple_codec_(&default_tuple_codec_) {}

DefaultExecutor::DefaultExecutor(Catalog &catalog, TableStorageManager &storage,
                                 const TupleCodec &tuple_codec)
    : catalog_(&catalog), storage_(&storage), default_tuple_codec_(),
      tuple_codec_(&tuple_codec) {}

Result<QueryResult> DefaultExecutor::Execute(const PhysicalPlan &plan,
                                             Transaction &transaction) {
  const auto active_status = require_active(transaction);
  if (!status_ok(active_status)) {
    return error_result<QueryResult>(active_status);
  }

  ExecutionDriver driver(*catalog_, *storage_, *tuple_codec_, transaction);
  return driver.Execute(plan);
}

} // namespace mattsql
