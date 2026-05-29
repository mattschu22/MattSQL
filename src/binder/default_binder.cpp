#include "mattsql/binder/default_binder.hpp"

#include "mattsql/common/result_utils.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

namespace mattsql {
namespace {

[[nodiscard]] std::string lowercase_key(std::string_view value) {
  std::string key;
  key.reserve(value.size());

  // Identifier lookup follows the catalog's case-insensitive key policy while
  // preserving source spelling in returned bound metadata.
  for (const char character : value) {
    key.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }

  return key;
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

  return SqlType::Null;
}

struct ColumnNameParts {
  std::string qualifier;
  std::string column;
};

[[nodiscard]] Result<ColumnNameParts> split_column_name(std::string_view name) {
  if (name.empty()) {
    return error_result<ColumnNameParts>(ErrorCode::BindError,
                                         "column name is required");
  }

  const auto dot = name.find('.');
  if (dot == std::string_view::npos) {
    return ok_result(ColumnNameParts{"", std::string(name)});
  }

  if (dot == 0 || dot + 1 == name.size() ||
      name.find('.', dot + 1) != std::string_view::npos) {
    return error_result<ColumnNameParts>(
        ErrorCode::BindError,
        "column references must be either column or table.column");
  }

  return ok_result(ColumnNameParts{std::string(name.substr(0, dot)),
                                   std::string(name.substr(dot + 1))});
}

[[nodiscard]] Result<ColumnSchema> resolve_column(const TableInfo &table,
                                                  std::string_view name) {
  auto parts = split_column_name(name);
  if (!status_ok(parts.status)) {
    return error_result<ColumnSchema>(std::move(parts.status));
  }

  if (!parts.value->qualifier.empty() &&
      lowercase_key(parts.value->qualifier) != lowercase_key(table.name)) {
    return error_result<ColumnSchema>(ErrorCode::BindError,
                                      "column reference uses unknown table: " +
                                          parts.value->qualifier);
  }

  const auto lookup_name = lowercase_key(parts.value->column);
  for (const auto &column : table.schema.columns) {
    if (lowercase_key(column.name) == lookup_name) {
      return ok_result(column);
    }
  }

  return error_result<ColumnSchema>(ErrorCode::BindError,
                                    "unknown column: " + std::string(name));
}

[[nodiscard]] BoundExpressionPtr make_literal(Value value, SqlType type) {
  auto bound = std::make_unique<BoundLiteralExpression>();
  bound->kind = BoundExpressionKind::Literal;
  bound->type = type;
  bound->value = std::move(value);
  return bound;
}

[[nodiscard]] BoundExpressionPtr make_bound_column(const TableInfo &table,
                                                   const ColumnSchema &column) {
  auto bound = std::make_unique<BoundColumnExpression>();
  bound->kind = BoundExpressionKind::Column;
  bound->type = column.type;
  bound->table_id = table.id;
  bound->column_id = column.id;
  bound->table_name = table.name;
  bound->column_name = column.name;
  return bound;
}

[[nodiscard]] bool is_boolean_integer_literal(const BoundExpression &expression) {
  if (expression.type != SqlType::Integer) {
    return false;
  }

  const auto *literal = dynamic_cast<const BoundLiteralExpression *>(&expression);
  if (literal == nullptr) {
    return false;
  }

  const auto *integer = std::get_if<std::int64_t>(&literal->value);
  return integer != nullptr && (*integer == 0 || *integer == 1);
}

[[nodiscard]] bool equality_comparable(const BoundExpression &left,
                                       const BoundExpression &right) {
  if (left.type == SqlType::Null || right.type == SqlType::Null) {
    return true;
  }
  if (left.type == right.type) {
    return true;
  }
  if (left.type == SqlType::Boolean && is_boolean_integer_literal(right)) {
    return true;
  }
  if (right.type == SqlType::Boolean && is_boolean_integer_literal(left)) {
    return true;
  }

  return false;
}

[[nodiscard]] bool ordering_comparable(SqlType left, SqlType right) {
  if (left != right) {
    return false;
  }

  return left == SqlType::Integer || left == SqlType::Text;
}

[[nodiscard]] bool coerce_boolean_integer_literal(BoundExpression &expression) {
  if (!is_boolean_integer_literal(expression)) {
    return false;
  }

  auto *literal = dynamic_cast<BoundLiteralExpression *>(&expression);
  const auto integer = std::get<std::int64_t>(literal->value);
  literal->type = SqlType::Boolean;
  literal->value = integer == 1;
  return true;
}

[[nodiscard]] Status coerce_to_column(BoundExpression &expression,
                                      const ColumnSchema &column) {
  if (expression.type == SqlType::Null) {
    if (column.nullable) {
      return {};
    }
    return error_status(ErrorCode::TypeMismatch,
                        "value type does not match column: " + column.name);
  }
  if (expression.type == column.type) {
    return {};
  }

  // The parser has no TRUE/FALSE literals yet, so allow 0/1 integer literals
  // for boolean columns and normalize them into typed boolean bound literals.
  if (column.type == SqlType::Boolean && coerce_boolean_integer_literal(expression)) {
    return {};
  }

  return error_status(ErrorCode::TypeMismatch,
                      "value type does not match column: " + column.name);
}

[[nodiscard]] bool is_arithmetic(BinaryOperator op) {
  return op == BinaryOperator::Add || op == BinaryOperator::Subtract ||
         op == BinaryOperator::Multiply || op == BinaryOperator::Divide;
}

[[nodiscard]] bool is_equality_comparison(BinaryOperator op) {
  return op == BinaryOperator::Equal || op == BinaryOperator::NotEqual;
}

[[nodiscard]] bool is_ordering_comparison(BinaryOperator op) {
  return op == BinaryOperator::Less || op == BinaryOperator::LessEqual ||
         op == BinaryOperator::Greater || op == BinaryOperator::GreaterEqual;
}

[[nodiscard]] bool is_boolean_operator(BinaryOperator op) {
  return op == BinaryOperator::And || op == BinaryOperator::Or;
}

[[nodiscard]] Result<BoundExpressionPtr> bind_expression(const Expression &expression,
                                                         const TableInfo *table);

[[nodiscard]] Result<BoundExpressionPtr> bind_column(const ColumnRef &expression,
                                                     const TableInfo *table) {
  if (table == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::BindError,
                                            "column reference requires a FROM table");
  }

  auto column = resolve_column(*table, expression.name);
  if (!status_ok(column.status)) {
    return error_result<BoundExpressionPtr>(std::move(column.status));
  }

  return ok_result(make_bound_column(*table, *column.value));
}

[[nodiscard]] Result<BoundExpressionPtr> bind_unary(const UnaryExpression &expression,
                                                    const TableInfo *table) {
  auto operand = bind_expression(*expression.operand, table);
  if (!status_ok(operand.status)) {
    return operand;
  }

  SqlType result_type = SqlType::Null;
  switch (expression.op) {
  case UnaryOperator::Plus:
  case UnaryOperator::Minus:
    if ((*operand.value)->type != SqlType::Integer) {
      return error_result<BoundExpressionPtr>(
          ErrorCode::TypeMismatch, "unary numeric operators require INTEGER");
    }
    result_type = SqlType::Integer;
    break;
  case UnaryOperator::Not:
    if ((*operand.value)->type != SqlType::Boolean) {
      return error_result<BoundExpressionPtr>(ErrorCode::TypeMismatch,
                                              "NOT requires BOOLEAN");
    }
    result_type = SqlType::Boolean;
    break;
  }

  auto bound = std::make_unique<BoundUnaryExpression>();
  bound->kind = BoundExpressionKind::Unary;
  bound->type = result_type;
  bound->op = expression.op;
  bound->operand = std::move(*operand.value);
  return ok_result<BoundExpressionPtr>(std::move(bound));
}

[[nodiscard]] Result<BoundExpressionPtr> bind_binary(const BinaryExpression &expression,
                                                     const TableInfo *table) {
  auto left = bind_expression(*expression.left, table);
  if (!status_ok(left.status)) {
    return left;
  }

  auto right = bind_expression(*expression.right, table);
  if (!status_ok(right.status)) {
    return right;
  }

  SqlType result_type = SqlType::Null;

  if (is_arithmetic(expression.op)) {
    if ((*left.value)->type != SqlType::Integer ||
        (*right.value)->type != SqlType::Integer) {
      return error_result<BoundExpressionPtr>(ErrorCode::TypeMismatch,
                                              "arithmetic operators require INTEGER");
    }
    result_type = SqlType::Integer;
  } else if (is_boolean_operator(expression.op)) {
    if ((*left.value)->type != SqlType::Boolean ||
        (*right.value)->type != SqlType::Boolean) {
      return error_result<BoundExpressionPtr>(ErrorCode::TypeMismatch,
                                              "boolean operators require BOOLEAN");
    }
    result_type = SqlType::Boolean;
  } else if (is_equality_comparison(expression.op)) {
    if (!equality_comparable(**left.value, **right.value)) {
      return error_result<BoundExpressionPtr>(
          ErrorCode::TypeMismatch,
          "equality comparison operands have incompatible types");
    }
    result_type = SqlType::Boolean;
  } else if (is_ordering_comparison(expression.op)) {
    if (!ordering_comparable((*left.value)->type, (*right.value)->type)) {
      return error_result<BoundExpressionPtr>(
          ErrorCode::TypeMismatch,
          "ordering comparison operands have incompatible types");
    }
    result_type = SqlType::Boolean;
  }

  auto bound = std::make_unique<BoundBinaryExpression>();
  bound->kind = BoundExpressionKind::Binary;
  bound->type = result_type;
  bound->left = std::move(*left.value);
  bound->op = expression.op;
  bound->right = std::move(*right.value);
  return ok_result<BoundExpressionPtr>(std::move(bound));
}

[[nodiscard]] Result<BoundExpressionPtr> bind_expression(const Expression &expression,
                                                         const TableInfo *table) {
  if (const auto *literal = dynamic_cast<const IntegerLiteral *>(&expression)) {
    return ok_result(make_literal(literal->value, SqlType::Integer));
  }
  if (const auto *literal = dynamic_cast<const StringLiteral *>(&expression)) {
    return ok_result(make_literal(literal->value, SqlType::Text));
  }
  if (const auto *literal = dynamic_cast<const BooleanLiteral *>(&expression)) {
    return ok_result(make_literal(literal->value, SqlType::Boolean));
  }
  if (dynamic_cast<const NullLiteral *>(&expression) != nullptr) {
    return ok_result(make_literal(NullValue{}, SqlType::Null));
  }
  if (const auto *column = dynamic_cast<const ColumnRef *>(&expression)) {
    return bind_column(*column, table);
  }
  if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression)) {
    return bind_unary(*unary, table);
  }
  if (const auto *binary = dynamic_cast<const BinaryExpression *>(&expression)) {
    return bind_binary(*binary, table);
  }
  if (dynamic_cast<const StarExpression *>(&expression) != nullptr) {
    return error_result<BoundExpressionPtr>(
        ErrorCode::BindError, "star can only appear in a SELECT projection");
  }

  return error_result<BoundExpressionPtr>(ErrorCode::BindError,
                                          "unsupported expression");
}

[[nodiscard]] Result<BoundStatementPtr>
bind_create_table(const CreateTableStatement &statement, Catalog &catalog) {
  if (statement.table_name.empty()) {
    return error_result<BoundStatementPtr>(ErrorCode::BindError,
                                           "table name is required");
  }
  if (statement.columns.empty()) {
    return error_result<BoundStatementPtr>(ErrorCode::BindError,
                                           "CREATE TABLE requires at least one column");
  }

  const auto existing = catalog.GetTable(statement.table_name);
  if (status_ok(existing.status)) {
    return error_result<BoundStatementPtr>(ErrorCode::AlreadyExists,
                                           "table already exists");
  }
  if (existing.status.code != ErrorCode::NotFound) {
    return error_result<BoundStatementPtr>(existing.status);
  }

  CreateTableRequest request;
  request.name = statement.table_name;
  request.schema.columns.reserve(statement.columns.size());

  std::unordered_set<std::string> column_names;
  for (std::size_t index = 0; index < statement.columns.size(); ++index) {
    if (index > std::numeric_limits<ColumnId>::max()) {
      return error_result<BoundStatementPtr>(ErrorCode::BindError,
                                             "table has too many columns");
    }

    const auto &parsed_column = statement.columns[index];
    if (parsed_column.name.empty()) {
      return error_result<BoundStatementPtr>(ErrorCode::BindError,
                                             "column name is required");
    }

    const auto column_key = lowercase_key(parsed_column.name);
    if (!column_names.insert(column_key).second) {
      return error_result<BoundStatementPtr>(ErrorCode::BindError,
                                             "duplicate column name");
    }

    ColumnSchema column;
    column.name = parsed_column.name;
    column.type = to_sql_type(parsed_column.type);
    column.id = static_cast<ColumnId>(index);
    request.schema.columns.push_back(std::move(column));
  }

  auto bound = std::make_unique<BoundCreateTableStatement>();
  bound->kind = BoundStatementKind::CreateTable;
  bound->request = std::move(request);
  return ok_result<BoundStatementPtr>(std::move(bound));
}

[[nodiscard]] Result<BoundStatementPtr> bind_insert(const InsertStatement &statement,
                                                    Catalog &catalog) {
  const auto table = catalog.GetTable(statement.table_name);
  if (!status_ok(table.status)) {
    return error_result<BoundStatementPtr>(table.status);
  }

  if (statement.values.size() != table.value->schema.columns.size()) {
    return error_result<BoundStatementPtr>(
        ErrorCode::BindError, "INSERT value count does not match table schema");
  }

  auto bound = std::make_unique<BoundInsertStatement>();
  bound->kind = BoundStatementKind::Insert;
  bound->table = *table.value;
  bound->values.reserve(statement.values.size());

  for (std::size_t index = 0; index < statement.values.size(); ++index) {
    auto expression = bind_expression(*statement.values[index], nullptr);
    if (!status_ok(expression.status)) {
      return error_result<BoundStatementPtr>(std::move(expression.status));
    }

    const auto &column = bound->table.schema.columns[index];
    const auto coercion_status = coerce_to_column(**expression.value, column);
    if (!status_ok(coercion_status)) {
      return error_result<BoundStatementPtr>(coercion_status);
    }

    bound->values.push_back(std::move(*expression.value));
  }

  return ok_result<BoundStatementPtr>(std::move(bound));
}

[[nodiscard]] Result<BoundStatementPtr> bind_select(const SelectStatement &statement,
                                                    Catalog &catalog) {
  auto bound = std::make_unique<BoundSelectStatement>();
  bound->kind = BoundStatementKind::Select;

  const TableInfo *table = nullptr;
  if (!statement.table_name.empty()) {
    const auto resolved_table = catalog.GetTable(statement.table_name);
    if (!status_ok(resolved_table.status)) {
      return error_result<BoundStatementPtr>(resolved_table.status);
    }
    bound->table = *resolved_table.value;
    table = &bound->table;
  }

  bound->projections.reserve(statement.projections.size());
  for (const auto &projection : statement.projections) {
    if (dynamic_cast<const StarExpression *>(projection.expression.get()) != nullptr) {
      if (table == nullptr) {
        return error_result<BoundStatementPtr>(ErrorCode::BindError,
                                               "SELECT * requires a FROM table");
      }

      // Expand stars during binding so planning/execution sees concrete column
      // references with stable catalog identifiers.
      for (const auto &column : table->schema.columns) {
        bound->projections.push_back(make_bound_column(*table, column));
      }
      continue;
    }

    auto expression = bind_expression(*projection.expression, table);
    if (!status_ok(expression.status)) {
      return error_result<BoundStatementPtr>(std::move(expression.status));
    }
    bound->projections.push_back(std::move(*expression.value));
  }

  if (statement.where != nullptr) {
    auto where = bind_expression(*statement.where, table);
    if (!status_ok(where.status)) {
      return error_result<BoundStatementPtr>(std::move(where.status));
    }
    if ((*where.value)->type != SqlType::Boolean) {
      return error_result<BoundStatementPtr>(ErrorCode::TypeMismatch,
                                             "WHERE predicate must be BOOLEAN");
    }

    bound->where = std::move(*where.value);
  }

  return ok_result<BoundStatementPtr>(std::move(bound));
}

} // namespace

Result<BoundStatementPtr> DefaultBinder::Bind(const Statement &statement,
                                              Catalog &catalog) {
  if (const auto *select = dynamic_cast<const SelectStatement *>(&statement)) {
    return bind_select(*select, catalog);
  }
  if (const auto *insert = dynamic_cast<const InsertStatement *>(&statement)) {
    return bind_insert(*insert, catalog);
  }
  if (const auto *create = dynamic_cast<const CreateTableStatement *>(&statement)) {
    return bind_create_table(*create, catalog);
  }

  return error_result<BoundStatementPtr>(ErrorCode::BindError, "unsupported statement");
}

} // namespace mattsql
