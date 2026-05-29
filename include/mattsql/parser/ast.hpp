#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mattsql {

// Base AST types are intentionally lightweight data nodes. Later execution and
// planning layers can visit or downcast these concrete statement/expression
// types without the parser owning any runtime behavior.
struct AstNode {
    /// Destroys an AST node through a base pointer.
    virtual ~AstNode() = default;
};

struct Statement : AstNode {
    /// Destroys a statement node through a base pointer.
    ~Statement() override = default;
};

struct Expression : AstNode {
    /// Destroys an expression node through a base pointer.
    ~Expression() override = default;
};

using StatementPtr = std::unique_ptr<Statement>;
using ExpressionPtr = std::unique_ptr<Expression>;

// Literal expressions store parsed values rather than raw token text.
struct IntegerLiteral final : Expression {
    /// Creates an integer literal from a parsed signed 64-bit value.
    explicit IntegerLiteral(std::int64_t value) : value(value) {}

    std::int64_t value;
};

struct StringLiteral final : Expression {
    /// Creates a string literal from a decoded SQL string value.
    explicit StringLiteral(std::string value) : value(std::move(value)) {}

    std::string value;
};

struct NullLiteral final : Expression {};

// ColumnRef currently stores the source-level name, including dotted names such
// as "users.id"; name resolution belongs to a later semantic analysis phase.
struct ColumnRef final : Expression {
    /// Creates a column reference from its source-level name.
    explicit ColumnRef(std::string name) : name(std::move(name)) {}

    std::string name;
};

enum class UnaryOperator {
    Plus,
    Minus,
    Not
};

// UnaryExpression owns its operand so expression trees have a single owner.
struct UnaryExpression final : Expression {
    /// Creates a unary expression that owns its operand subtree.
    UnaryExpression(UnaryOperator op, ExpressionPtr operand)
        : op(op), operand(std::move(operand)) {}

    UnaryOperator op;
    ExpressionPtr operand;
};

enum class BinaryOperator {
    Add,
    Subtract,
    Multiply,
    Divide,

    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    And,
    Or
};

// BinaryExpression is used for arithmetic, comparison, and boolean operators.
struct BinaryExpression final : Expression {
    /// Creates a binary expression that owns both operand subtrees.
    BinaryExpression(ExpressionPtr left, BinaryOperator op, ExpressionPtr right)
        : left(std::move(left)), op(op), right(std::move(right)) {}

    ExpressionPtr left;
    BinaryOperator op;
    ExpressionPtr right;
};

struct StarExpression final : Expression {};

struct SelectProjection {
    ExpressionPtr expression;
    std::string alias;
};

// SELECT supports a projection list, an optional FROM table, and an optional
// WHERE predicate. An empty table_name means FROM was omitted.
struct SelectStatement final : Statement {
    std::vector<SelectProjection> projections;
    std::string table_name;
    ExpressionPtr where;
};

// INSERT currently supports the compact form: INSERT INTO table VALUES (...).
struct InsertStatement final : Statement {
    std::string table_name;
    std::vector<ExpressionPtr> values;
};

enum class TypeName {
    Int,
    Text,
    Bool
};

struct ColumnDefinition {
    std::string name;
    TypeName type;
};

// CREATE TABLE stores parsed column definitions only; constraints and indexes
// can be layered into this node later.
struct CreateTableStatement final : Statement {
    std::string table_name;
    std::vector<ColumnDefinition> columns;
};

}  // namespace mattsql
