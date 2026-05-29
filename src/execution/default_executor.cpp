#include "mattsql/execution/default_executor.hpp"

#include "mattsql/common/result_utils.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mattsql {
namespace {

struct RowContext {
  const TableSchema *schema = nullptr;
  const std::vector<Value> *row = nullptr;
};

struct ExecutionResult {
  std::vector<std::string> columns;
  TableSchema schema;
  std::vector<std::vector<Value>> rows;
};

[[nodiscard]] bool is_null(const Value &value) {
  return std::holds_alternative<NullValue>(value);
}

[[nodiscard]] Result<std::int64_t> require_integer(const Value &value,
                                                   std::string_view context) {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return ok_result(*integer);
  }

  return error_result<std::int64_t>(
      ErrorCode::TypeMismatch, std::string(context) + " requires INTEGER operands");
}

[[nodiscard]] Result<bool> require_boolean(const Value &value,
                                           std::string_view context) {
  if (const auto *boolean = std::get_if<bool>(&value)) {
    return ok_result(*boolean);
  }

  return error_result<bool>(ErrorCode::TypeMismatch,
                            std::string(context) + " requires BOOLEAN operands");
}

[[nodiscard]] Result<int> compare_values(const Value &left, const Value &right) {
  if (is_null(left) || is_null(right)) {
    return error_result<int>(ErrorCode::ExecutionError, "cannot compare NULL values");
  }

  const auto left_integral =
      std::holds_alternative<std::int64_t>(left) || std::holds_alternative<bool>(left);
  const auto right_integral = std::holds_alternative<std::int64_t>(right) ||
                              std::holds_alternative<bool>(right);
  if (left_integral && right_integral) {
    const auto left_value = std::holds_alternative<bool>(left)
                                ? (std::get<bool>(left) ? 1 : 0)
                                : std::get<std::int64_t>(left);
    const auto right_value = std::holds_alternative<bool>(right)
                                 ? (std::get<bool>(right) ? 1 : 0)
                                 : std::get<std::int64_t>(right);
    if (left_value < right_value) {
      return ok_result(-1);
    }
    if (right_value < left_value) {
      return ok_result(1);
    }
    return ok_result(0);
  }

  const auto *left_string = std::get_if<std::string>(&left);
  const auto *right_string = std::get_if<std::string>(&right);
  if (left_string != nullptr && right_string != nullptr) {
    if (*left_string < *right_string) {
      return ok_result(-1);
    }
    if (*right_string < *left_string) {
      return ok_result(1);
    }
    return ok_result(0);
  }

  return error_result<int>(ErrorCode::TypeMismatch,
                           "cannot compare values with different types");
}

[[nodiscard]] Result<Value> evaluate_expression(const BoundExpression &expression,
                                                const RowContext &context);

[[nodiscard]] Result<Value> evaluate_unary(const BoundUnaryExpression &expression,
                                           const RowContext &context) {
  if (expression.operand == nullptr) {
    return error_result<Value>(ErrorCode::ExecutionError,
                               "unary expression is missing its operand");
  }

  auto operand = evaluate_expression(*expression.operand, context);
  if (!status_ok(operand.status)) {
    return operand;
  }

  switch (expression.op) {
  case UnaryOperator::Plus: {
    auto integer = require_integer(*operand.value, "unary plus");
    if (!status_ok(integer.status)) {
      return error_result<Value>(integer.status);
    }
    return ok_result<Value>(*integer.value);
  }
  case UnaryOperator::Minus: {
    auto integer = require_integer(*operand.value, "unary minus");
    if (!status_ok(integer.status)) {
      return error_result<Value>(integer.status);
    }
    return ok_result<Value>(-*integer.value);
  }
  case UnaryOperator::Not: {
    auto boolean = require_boolean(*operand.value, "NOT");
    if (!status_ok(boolean.status)) {
      return error_result<Value>(boolean.status);
    }
    return ok_result<Value>(!*boolean.value);
  }
  }

  return error_result<Value>(ErrorCode::ExecutionError, "unknown unary operator");
}

[[nodiscard]] Result<Value> evaluate_binary(const BoundBinaryExpression &expression,
                                            const RowContext &context) {
  if (expression.left == nullptr || expression.right == nullptr) {
    return error_result<Value>(ErrorCode::ExecutionError,
                               "binary expression is missing an operand");
  }

  if (expression.op == BinaryOperator::And) {
    auto left = evaluate_expression(*expression.left, context);
    if (!status_ok(left.status)) {
      return left;
    }
    auto left_boolean = require_boolean(*left.value, "AND");
    if (!status_ok(left_boolean.status)) {
      return error_result<Value>(left_boolean.status);
    }
    if (!*left_boolean.value) {
      return ok_result<Value>(false);
    }

    auto right = evaluate_expression(*expression.right, context);
    if (!status_ok(right.status)) {
      return right;
    }
    auto right_boolean = require_boolean(*right.value, "AND");
    if (!status_ok(right_boolean.status)) {
      return error_result<Value>(right_boolean.status);
    }
    return ok_result<Value>(*right_boolean.value);
  }

  if (expression.op == BinaryOperator::Or) {
    auto left = evaluate_expression(*expression.left, context);
    if (!status_ok(left.status)) {
      return left;
    }
    auto left_boolean = require_boolean(*left.value, "OR");
    if (!status_ok(left_boolean.status)) {
      return error_result<Value>(left_boolean.status);
    }
    if (*left_boolean.value) {
      return ok_result<Value>(true);
    }

    auto right = evaluate_expression(*expression.right, context);
    if (!status_ok(right.status)) {
      return right;
    }
    auto right_boolean = require_boolean(*right.value, "OR");
    if (!status_ok(right_boolean.status)) {
      return error_result<Value>(right_boolean.status);
    }
    return ok_result<Value>(*right_boolean.value);
  }

  auto left = evaluate_expression(*expression.left, context);
  if (!status_ok(left.status)) {
    return left;
  }

  auto right = evaluate_expression(*expression.right, context);
  if (!status_ok(right.status)) {
    return right;
  }

  switch (expression.op) {
  case BinaryOperator::Add: {
    auto left_integer = require_integer(*left.value, "addition");
    auto right_integer = require_integer(*right.value, "addition");
    if (!status_ok(left_integer.status)) {
      return error_result<Value>(left_integer.status);
    }
    if (!status_ok(right_integer.status)) {
      return error_result<Value>(right_integer.status);
    }
    return ok_result<Value>(*left_integer.value + *right_integer.value);
  }
  case BinaryOperator::Subtract: {
    auto left_integer = require_integer(*left.value, "subtraction");
    auto right_integer = require_integer(*right.value, "subtraction");
    if (!status_ok(left_integer.status)) {
      return error_result<Value>(left_integer.status);
    }
    if (!status_ok(right_integer.status)) {
      return error_result<Value>(right_integer.status);
    }
    return ok_result<Value>(*left_integer.value - *right_integer.value);
  }
  case BinaryOperator::Multiply: {
    auto left_integer = require_integer(*left.value, "multiplication");
    auto right_integer = require_integer(*right.value, "multiplication");
    if (!status_ok(left_integer.status)) {
      return error_result<Value>(left_integer.status);
    }
    if (!status_ok(right_integer.status)) {
      return error_result<Value>(right_integer.status);
    }
    return ok_result<Value>(*left_integer.value * *right_integer.value);
  }
  case BinaryOperator::Divide: {
    auto left_integer = require_integer(*left.value, "division");
    auto right_integer = require_integer(*right.value, "division");
    if (!status_ok(left_integer.status)) {
      return error_result<Value>(left_integer.status);
    }
    if (!status_ok(right_integer.status)) {
      return error_result<Value>(right_integer.status);
    }
    if (*right_integer.value == 0) {
      return error_result<Value>(ErrorCode::ExecutionError, "division by zero");
    }
    return ok_result<Value>(*left_integer.value / *right_integer.value);
  }
  case BinaryOperator::Equal:
    if (is_null(*left.value) || is_null(*right.value)) {
      return ok_result<Value>(false);
    }
    if (auto comparison = compare_values(*left.value, *right.value);
        status_ok(comparison.status)) {
      return ok_result<Value>(*comparison.value == 0);
    } else {
      return error_result<Value>(comparison.status);
    }
  case BinaryOperator::NotEqual:
    if (is_null(*left.value) || is_null(*right.value)) {
      return ok_result<Value>(false);
    }
    if (auto comparison = compare_values(*left.value, *right.value);
        status_ok(comparison.status)) {
      return ok_result<Value>(*comparison.value != 0);
    } else {
      return error_result<Value>(comparison.status);
    }
  case BinaryOperator::Less:
  case BinaryOperator::LessEqual:
  case BinaryOperator::Greater:
  case BinaryOperator::GreaterEqual: {
    auto comparison = compare_values(*left.value, *right.value);
    if (!status_ok(comparison.status)) {
      return error_result<Value>(comparison.status);
    }

    switch (expression.op) {
    case BinaryOperator::Less:
      return ok_result<Value>(*comparison.value < 0);
    case BinaryOperator::LessEqual:
      return ok_result<Value>(*comparison.value <= 0);
    case BinaryOperator::Greater:
      return ok_result<Value>(*comparison.value > 0);
    case BinaryOperator::GreaterEqual:
      return ok_result<Value>(*comparison.value >= 0);
    default:
      break;
    }
  }
  case BinaryOperator::And:
  case BinaryOperator::Or:
    break;
  }

  return error_result<Value>(ErrorCode::ExecutionError, "unknown binary operator");
}

[[nodiscard]] Result<Value> evaluate_expression(const BoundExpression &expression,
                                                const RowContext &context) {
  if (const auto *literal = dynamic_cast<const BoundLiteralExpression *>(&expression)) {
    return ok_result(literal->value);
  }

  if (const auto *column = dynamic_cast<const BoundColumnExpression *>(&expression)) {
    if (context.schema == nullptr || context.row == nullptr) {
      return error_result<Value>(ErrorCode::ExecutionError,
                                 "column expression requires a row context");
    }
    if (column->column_id >= context.row->size() ||
        column->column_id >= context.schema->columns.size()) {
      return error_result<Value>(ErrorCode::ExecutionError,
                                 "column id is out of range");
    }
    return ok_result((*context.row)[column->column_id]);
  }

  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&expression)) {
    return evaluate_unary(*unary, context);
  }

  if (const auto *binary = dynamic_cast<const BoundBinaryExpression *>(&expression)) {
    return evaluate_binary(*binary, context);
  }

  return error_result<Value>(ErrorCode::ExecutionError, "unsupported expression");
}

[[nodiscard]] std::string projection_name(const BoundExpression &expression,
                                          std::size_t index) {
  if (const auto *column = dynamic_cast<const BoundColumnExpression *>(&expression)) {
    return column->column_name;
  }

  return "expr" + std::to_string(index + 1);
}

[[nodiscard]] Result<std::vector<Value>>
evaluate_expressions(const std::vector<BoundExpressionPtr> &expressions,
                     const RowContext &context) {
  std::vector<Value> values;
  values.reserve(expressions.size());

  for (const auto &expression : expressions) {
    if (expression == nullptr) {
      return error_result<std::vector<Value>>(ErrorCode::ExecutionError,
                                              "expression cannot be null");
    }

    auto value = evaluate_expression(*expression, context);
    if (!status_ok(value.status)) {
      return error_result<std::vector<Value>>(std::move(value.status));
    }
    values.push_back(std::move(*value.value));
  }

  return ok_result(std::move(values));
}

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
    if (!status_ok(result.status)) {
      return error_result<QueryResult>(std::move(result.status));
    }

    return ok_result(to_query_result(std::move(*result.value)));
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
    if (!plan.children.empty()) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "CREATE TABLE cannot have children");
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
    if (!status_ok(table.status)) {
      return error_result<ExecutionResult>(std::move(table.status));
    }

    auto heap_root = storage_.CreateHeap(transaction_, *table.value);
    if (!status_ok(heap_root.status)) {
      (void)catalog_.DropTable(table.value->name);
      return error_result<ExecutionResult>(std::move(heap_root.status));
    }

    const auto root_status =
        catalog_.SetTableHeapRoot(table.value->id, *heap_root.value);
    if (!status_ok(root_status)) {
      return error_result<ExecutionResult>(root_status);
    }

    return ok_result(ExecutionResult{});
  }

  Result<ExecutionResult> execute_insert(const PhysicalInsert &plan) {
    if (plan.children.size() != 1 || plan.children[0] == nullptr) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "INSERT requires one input");
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
    if (!status_ok(input.status)) {
      return input;
    }

    auto heap = storage_.OpenHeap(plan.storage);
    if (!status_ok(heap.status)) {
      return error_result<ExecutionResult>(std::move(heap.status));
    }

    for (const auto &row : input.value->rows) {
      if (row.size() != plan.table.schema.columns.size()) {
        return error_result<ExecutionResult>(
            ErrorCode::ExecutionError, "INSERT row width does not match table schema");
      }

      auto tuple = tuple_codec_.Encode(plan.table.schema, row);
      if (!status_ok(tuple.status)) {
        return error_result<ExecutionResult>(std::move(tuple.status));
      }

      ConstBufferView record{std::span<const std::byte>(tuple.value->bytes.data(),
                                                        tuple.value->bytes.size())};
      auto inserted = (*heap.value)->Insert(transaction_, record);
      if (!status_ok(inserted.status)) {
        return error_result<ExecutionResult>(std::move(inserted.status));
      }
    }

    return ok_result(ExecutionResult{});
  }

  Result<ExecutionResult> execute_projection(const PhysicalProjection &plan) {
    if (plan.children.size() != 1 || plan.children[0] == nullptr) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "projection requires one input");
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
    if (!status_ok(input.status)) {
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
        column.name = projection_name(*projection, index);
      }
      column.type = projection->type;
      column.id = static_cast<ColumnId>(index);
      output.columns.push_back(column.name);
      output.schema.columns.push_back(std::move(column));
    }

    output.rows.reserve(input.value->rows.size());
    for (const auto &row : input.value->rows) {
      const RowContext context{&input.value->schema, &row};
      auto projected = evaluate_expressions(plan.projections, context);
      if (!status_ok(projected.status)) {
        return error_result<ExecutionResult>(std::move(projected.status));
      }
      output.rows.push_back(std::move(*projected.value));
    }

    return ok_result(std::move(output));
  }

  Result<ExecutionResult> execute_filter(const PhysicalFilter &plan) {
    if (plan.children.size() != 1 || plan.children[0] == nullptr) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "filter requires one input");
    }
    if (plan.predicate == nullptr) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "filter requires a predicate");
    }

    auto input = execute_plan(*plan.children[0]);
    if (!status_ok(input.status)) {
      return input;
    }

    ExecutionResult output;
    output.columns = input.value->columns;
    output.schema = input.value->schema;
    for (const auto &row : input.value->rows) {
      const RowContext context{&input.value->schema, &row};
      auto predicate = evaluate_expression(*plan.predicate, context);
      if (!status_ok(predicate.status)) {
        return error_result<ExecutionResult>(std::move(predicate.status));
      }

      auto include = require_boolean(*predicate.value, "filter");
      if (!status_ok(include.status)) {
        return error_result<ExecutionResult>(std::move(include.status));
      }
      if (*include.value) {
        output.rows.push_back(row);
      }
    }

    return ok_result(std::move(output));
  }

  Result<ExecutionResult> execute_values(const PhysicalValues &plan) {
    if (!plan.children.empty()) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "VALUES cannot have children");
    }

    ExecutionResult output;
    for (const auto &row : plan.rows) {
      const RowContext context;
      auto values = evaluate_expressions(row, context);
      if (!status_ok(values.status)) {
        return error_result<ExecutionResult>(std::move(values.status));
      }
      output.rows.push_back(std::move(*values.value));
    }

    return ok_result(std::move(output));
  }

  Result<ExecutionResult> execute_seq_scan(const PhysicalSeqScan &plan) {
    if (!plan.children.empty()) {
      return error_result<ExecutionResult>(ErrorCode::ExecutionError,
                                           "SeqScan cannot have children");
    }
    if (plan.storage.method != TableStorageMethod::Heap) {
      return error_result<ExecutionResult>(ErrorCode::NotSupported,
                                           "only heap table storage is supported");
    }

    auto heap = storage_.OpenHeap(plan.storage);
    if (!status_ok(heap.status)) {
      return error_result<ExecutionResult>(std::move(heap.status));
    }

    auto cursor = (*heap.value)->Scan(transaction_);
    if (!status_ok(cursor.status)) {
      return error_result<ExecutionResult>(std::move(cursor.status));
    }

    ExecutionResult output;
    output.schema = plan.table.schema;
    output.columns.reserve(plan.table.schema.columns.size());
    for (const auto &column : plan.table.schema.columns) {
      output.columns.push_back(column.name);
    }

    while (true) {
      auto record = (*cursor.value)->Next();
      if (record.status.code == ErrorCode::NotFound) {
        break;
      }
      if (!status_ok(record.status)) {
        return error_result<ExecutionResult>(std::move(record.status));
      }

      auto row = tuple_codec_.Decode(plan.table.schema, record.value->bytes);
      if (!status_ok(row.status)) {
        return error_result<ExecutionResult>(std::move(row.status));
      }
      output.rows.push_back(std::move(*row.value));
    }

    return ok_result(std::move(output));
  }

  Catalog &catalog_;
  TableStorageManager &storage_;
  const TupleCodec &tuple_codec_;
  Transaction &transaction_;
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
