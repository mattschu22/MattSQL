#include "mattsql/engine.hpp"

#include "mattsql/catalog/schema.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/lexer/lexer.hpp"
#include "mattsql/parser/ast.hpp"
#include "mattsql/parser/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace mattsql {
namespace {

struct StoredTable {
  std::string key;
  TableSchema schema;
  std::unordered_map<std::string, std::size_t> column_indexes;
  std::vector<std::vector<Value>> rows;
};

struct EvaluationContext {
  const StoredTable *table = nullptr;
  const std::vector<Value> *row = nullptr;
};

[[nodiscard]] std::string lowercase_key(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());

  for (const char character : value) {
    lowered.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }

  return lowered;
}

[[nodiscard]] std::string unqualified_name(std::string_view name) {
  const auto dot = name.rfind('.');
  if (dot == std::string_view::npos) {
    return std::string(name);
  }

  return std::string(name.substr(dot + 1));
}

[[nodiscard]] SqlType to_sql_type(TypeName type) {
  switch (type) {
  case TypeName::Int:
    return SqlType::Integer;
  case TypeName::Text:
    return SqlType::Text;
  case TypeName::Bool:
    return SqlType::Boolean;
  }

  throw std::logic_error("unknown parser type name");
}

[[nodiscard]] bool is_null(const Value &value) {
  return std::holds_alternative<NullValue>(value);
}

[[nodiscard]] std::int64_t require_integer(const Value &value,
                                           std::string_view context) {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return *integer;
  }

  throw std::runtime_error(std::string(context) + " requires integer operands");
}

[[nodiscard]] bool require_boolean(const Value &value, std::string_view context) {
  if (const auto *boolean = std::get_if<bool>(&value)) {
    return *boolean;
  }
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return *integer != 0;
  }

  throw std::runtime_error(std::string(context) + " requires boolean operands");
}

[[nodiscard]] std::size_t find_column(const StoredTable &table,
                                      std::string_view column_ref) {
  auto lookup_name = column_ref;
  const auto dot = column_ref.rfind('.');
  if (dot != std::string_view::npos) {
    const auto table_name = column_ref.substr(0, dot);
    if (lowercase_key(table_name) != table.key) {
      throw std::runtime_error("column reference uses unknown table: " +
                               std::string(table_name));
    }
    lookup_name = column_ref.substr(dot + 1);
  }

  const auto key = lowercase_key(lookup_name);
  const auto column_iter = table.column_indexes.find(key);
  if (column_iter != table.column_indexes.end()) {
    return column_iter->second;
  }

  throw std::runtime_error("unknown column: " + std::string(column_ref));
}

[[nodiscard]] int compare_values(const Value &left, const Value &right) {
  if (is_null(left) || is_null(right)) {
    throw std::runtime_error("cannot compare NULL values");
  }

  const auto left_is_integral =
      std::holds_alternative<std::int64_t>(left) || std::holds_alternative<bool>(left);
  const auto right_is_integral = std::holds_alternative<std::int64_t>(right) ||
                                 std::holds_alternative<bool>(right);
  if (left_is_integral && right_is_integral) {
    const auto left_value = std::holds_alternative<bool>(left)
                                ? (std::get<bool>(left) ? 1 : 0)
                                : std::get<std::int64_t>(left);
    const auto right_value = std::holds_alternative<bool>(right)
                                 ? (std::get<bool>(right) ? 1 : 0)
                                 : std::get<std::int64_t>(right);
    if (left_value < right_value) {
      return -1;
    }
    if (right_value < left_value) {
      return 1;
    }
    return 0;
  }

  if (const auto *left_string = std::get_if<std::string>(&left)) {
    const auto *right_string = std::get_if<std::string>(&right);
    if (right_string == nullptr) {
      throw std::runtime_error("cannot compare values with different types");
    }
    if (*left_string < *right_string) {
      return -1;
    }
    if (*right_string < *left_string) {
      return 1;
    }
    return 0;
  }

  throw std::runtime_error("cannot compare values with different types");
}

[[nodiscard]] Value evaluate_expression(const Expression &expression,
                                        const EvaluationContext &context);

[[nodiscard]] Value evaluate_unary(const UnaryExpression &expression,
                                   const EvaluationContext &context) {
  const auto operand = evaluate_expression(*expression.operand, context);

  switch (expression.op) {
  case UnaryOperator::Plus:
    return require_integer(operand, "unary plus");
  case UnaryOperator::Minus:
    return -require_integer(operand, "unary minus");
  case UnaryOperator::Not:
    return !require_boolean(operand, "NOT");
  }

  throw std::logic_error("unknown unary operator");
}

[[nodiscard]] Value evaluate_binary(const BinaryExpression &expression,
                                    const EvaluationContext &context) {
  if (expression.op == BinaryOperator::And) {
    const auto left = evaluate_expression(*expression.left, context);
    if (!require_boolean(left, "AND")) {
      return false;
    }
    return require_boolean(evaluate_expression(*expression.right, context), "AND");
  }

  if (expression.op == BinaryOperator::Or) {
    const auto left = evaluate_expression(*expression.left, context);
    if (require_boolean(left, "OR")) {
      return true;
    }
    return require_boolean(evaluate_expression(*expression.right, context), "OR");
  }

  const auto left = evaluate_expression(*expression.left, context);
  const auto right = evaluate_expression(*expression.right, context);

  switch (expression.op) {
  case BinaryOperator::Add:
    return require_integer(left, "addition") + require_integer(right, "addition");
  case BinaryOperator::Subtract:
    return require_integer(left, "subtraction") - require_integer(right, "subtraction");
  case BinaryOperator::Multiply:
    return require_integer(left, "multiplication") *
           require_integer(right, "multiplication");
  case BinaryOperator::Divide: {
    const auto divisor = require_integer(right, "division");
    if (divisor == 0) {
      throw std::runtime_error("division by zero");
    }
    return require_integer(left, "division") / divisor;
  }
  case BinaryOperator::Equal:
    if (is_null(left) || is_null(right)) {
      return false;
    }
    return compare_values(left, right) == 0;
  case BinaryOperator::NotEqual:
    if (is_null(left) || is_null(right)) {
      return false;
    }
    return compare_values(left, right) != 0;
  case BinaryOperator::Less:
    return compare_values(left, right) < 0;
  case BinaryOperator::LessEqual:
    return compare_values(left, right) <= 0;
  case BinaryOperator::Greater:
    return compare_values(left, right) > 0;
  case BinaryOperator::GreaterEqual:
    return compare_values(left, right) >= 0;
  case BinaryOperator::And:
  case BinaryOperator::Or:
    break;
  }

  throw std::logic_error("unknown binary operator");
}

[[nodiscard]] Value evaluate_expression(const Expression &expression,
                                        const EvaluationContext &context) {
  if (const auto *literal = dynamic_cast<const IntegerLiteral *>(&expression)) {
    return literal->value;
  }
  if (const auto *literal = dynamic_cast<const StringLiteral *>(&expression)) {
    return literal->value;
  }
  if (const auto *literal = dynamic_cast<const BooleanLiteral *>(&expression)) {
    return literal->value;
  }
  if (dynamic_cast<const NullLiteral *>(&expression) != nullptr) {
    return NullValue{};
  }
  if (const auto *column = dynamic_cast<const ColumnRef *>(&expression)) {
    if (context.table == nullptr || context.row == nullptr) {
      throw std::runtime_error("column reference requires a FROM table");
    }
    const auto index = find_column(*context.table, column->name);
    return (*context.row)[index];
  }
  if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression)) {
    return evaluate_unary(*unary, context);
  }
  if (const auto *binary = dynamic_cast<const BinaryExpression *>(&expression)) {
    return evaluate_binary(*binary, context);
  }
  if (dynamic_cast<const StarExpression *>(&expression) != nullptr) {
    throw std::runtime_error("star cannot be evaluated as a scalar expression");
  }

  throw std::runtime_error("unsupported expression");
}

[[nodiscard]] std::string default_projection_name(const Expression &expression,
                                                  std::size_t index) {
  if (const auto *column = dynamic_cast<const ColumnRef *>(&expression)) {
    return unqualified_name(column->name);
  }
  return "expr" + std::to_string(index + 1);
}

[[nodiscard]] Value coerce_for_column(const Value &value, const ColumnSchema &column) {
  if (is_null(value)) {
    if (!column.nullable) {
      throw std::runtime_error("column cannot be NULL: " + column.name);
    }
    return value;
  }

  switch (column.type) {
  case SqlType::Integer:
    if (std::holds_alternative<std::int64_t>(value)) {
      return value;
    }
    break;
  case SqlType::Text:
    if (std::holds_alternative<std::string>(value)) {
      return value;
    }
    break;
  case SqlType::Boolean:
    if (std::holds_alternative<bool>(value)) {
      return value;
    }
    if (const auto *integer = std::get_if<std::int64_t>(&value)) {
      if (*integer == 0 || *integer == 1) {
        return *integer == 1;
      }
    }
    break;
  case SqlType::Null:
    break;
  }

  throw std::runtime_error("value type does not match column: " + column.name);
}

} // namespace

struct Engine::Impl {
  std::unordered_map<std::string, StoredTable> tables;
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() = default;

Engine::Engine(Engine &&) noexcept = default;

Engine &Engine::operator=(Engine &&) noexcept = default;

QueryResult Engine::execute(std::string_view sql) {
  Lexer lexer(sql);
  Parser parser(lexer.Tokenize());
  const auto statement = parser.ParseStatement();

  if (const auto *create =
          dynamic_cast<const CreateTableStatement *>(statement.get())) {
    const auto table_key = lowercase_key(create->table_name);
    if (impl_->tables.contains(table_key)) {
      throw std::runtime_error("table already exists: " + create->table_name);
    }
    if (create->columns.empty()) {
      throw std::runtime_error("CREATE TABLE requires at least one column");
    }

    StoredTable table;
    table.key = table_key;

    std::unordered_set<std::string> column_names;
    for (std::size_t index = 0; index < create->columns.size(); ++index) {
      if (index > std::numeric_limits<ColumnId>::max()) {
        throw std::runtime_error("too many columns in table: " + create->table_name);
      }

      const auto &parsed_column = create->columns[index];
      const auto column_key = lowercase_key(parsed_column.name);
      if (!column_names.insert(column_key).second) {
        throw std::runtime_error("duplicate column name: " + parsed_column.name);
      }

      ColumnSchema column;
      column.name = parsed_column.name;
      column.type = to_sql_type(parsed_column.type);
      column.id = static_cast<ColumnId>(index);
      table.column_indexes.emplace(column_key, index);
      table.schema.columns.push_back(std::move(column));
    }

    impl_->tables.emplace(table_key, std::move(table));
    return {};
  }

  if (const auto *insert = dynamic_cast<const InsertStatement *>(statement.get())) {
    const auto table_key = lowercase_key(insert->table_name);
    auto table_iter = impl_->tables.find(table_key);
    if (table_iter == impl_->tables.end()) {
      throw std::runtime_error("unknown table: " + insert->table_name);
    }

    auto &table = table_iter->second;
    if (insert->values.size() != table.schema.columns.size()) {
      throw std::runtime_error("INSERT value count does not match table schema");
    }

    std::vector<Value> row;
    row.reserve(insert->values.size());

    const EvaluationContext context;
    for (std::size_t index = 0; index < insert->values.size(); ++index) {
      const auto value = evaluate_expression(*insert->values[index], context);
      row.push_back(coerce_for_column(value, table.schema.columns[index]));
    }

    table.rows.push_back(std::move(row));
    return {};
  }

  const auto *select = dynamic_cast<const SelectStatement *>(statement.get());
  if (select == nullptr) {
    throw std::runtime_error("unsupported SQL statement");
  }

  const StoredTable *table = nullptr;
  if (!select->table_name.empty()) {
    const auto table_key = lowercase_key(select->table_name);
    const auto table_iter = impl_->tables.find(table_key);
    if (table_iter == impl_->tables.end()) {
      throw std::runtime_error("unknown table: " + select->table_name);
    }
    table = &table_iter->second;
  }

  QueryResult result;
  for (std::size_t index = 0; index < select->projections.size(); ++index) {
    const auto &projection = select->projections[index];

    if (dynamic_cast<const StarExpression *>(projection.expression.get()) != nullptr) {
      if (table == nullptr) {
        throw std::runtime_error("SELECT * requires a FROM table");
      }
      for (const auto &column : table->schema.columns) {
        result.columns.push_back(column.name);
      }
      continue;
    }

    if (!projection.alias.empty()) {
      result.columns.push_back(projection.alias);
    } else {
      result.columns.push_back(default_projection_name(*projection.expression, index));
    }
  }

  auto append_row = [&](const EvaluationContext &context) {
    std::vector<Value> output_row;
    output_row.reserve(result.columns.size());

    for (const auto &projection : select->projections) {
      if (dynamic_cast<const StarExpression *>(projection.expression.get()) !=
          nullptr) {
        for (const auto &value : *context.row) {
          output_row.push_back(value);
        }
        continue;
      }

      output_row.push_back(evaluate_expression(*projection.expression, context));
    }

    result.rows.push_back(std::move(output_row));
  };

  if (table == nullptr) {
    const EvaluationContext context;
    if (select->where == nullptr ||
        require_boolean(evaluate_expression(*select->where, context), "WHERE")) {
      append_row(context);
    }
    return result;
  }

  for (const auto &row : table->rows) {
    const EvaluationContext context{table, &row};
    if (select->where != nullptr &&
        !require_boolean(evaluate_expression(*select->where, context), "WHERE")) {
      continue;
    }
    append_row(context);
  }

  return result;
}

std::string version() { return "0.1.0"; }

} // namespace mattsql
