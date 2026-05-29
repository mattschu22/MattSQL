#pragma once

#include <string>
#include <vector>

#include "mattsql/lexer/token.hpp"
#include "mattsql/parser/ast.hpp"
#include "mattsql/parser/parse_error.hpp"

namespace mattsql {

class Parser {
public:
    /// Creates a parser that owns the provided token stream.
    explicit Parser(std::vector<Token> tokens);

    /// Parses one complete SQL statement and rejects trailing non-EOF tokens.
    [[nodiscard]] StatementPtr ParseStatement();

private:
    /// Returns true when the current token is the final EOF sentinel.
    [[nodiscard]] bool IsAtEnd() const;

    /// Returns the current token without consuming it.
    [[nodiscard]] const Token& Peek() const;

    /// Returns the most recently consumed token.
    [[nodiscard]] const Token& Previous() const;

    /// Consumes and returns the current token unless it is EOF.
    const Token& Advance();

    /// Returns true when the current token has the requested type.
    [[nodiscard]] bool Check(TokenType type) const;

    /// Consumes the current token only when it has the requested type.
    [[nodiscard]] bool Match(TokenType type);

    /// Consumes a required token or throws ParseError with the supplied message.
    const Token& Consume(TokenType type, const std::string& message);

    /// Parses a SELECT statement after the SELECT keyword has been consumed.
    [[nodiscard]] StatementPtr ParseSelectStatement();

    /// Parses an INSERT statement after the INSERT keyword has been consumed.
    [[nodiscard]] StatementPtr ParseInsertStatement();

    /// Parses a CREATE TABLE statement after the CREATE keyword has been consumed.
    [[nodiscard]] StatementPtr ParseCreateTableStatement();

    /// Parses a comma-separated expression list.
    [[nodiscard]] std::vector<ExpressionPtr> ParseExpressionList();

    /// Parses a comma-separated SELECT projection list with optional aliases.
    [[nodiscard]] std::vector<SelectProjection> ParseSelectProjectionList();

    /// Parses an expression using precedence climbing.
    [[nodiscard]] ExpressionPtr ParseExpression(int min_precedence = 0);

    /// Parses prefix unary expressions or delegates to primary parsing.
    [[nodiscard]] ExpressionPtr ParsePrefixExpression();

    /// Parses literals, column references, stars, and parenthesized expressions.
    [[nodiscard]] ExpressionPtr ParsePrimaryExpression();

    /// Returns the binary precedence for a token type, or -1 when not binary.
    [[nodiscard]] static int GetBinaryPrecedence(TokenType type);

    /// Converts a binary operator token type into its AST operator enum.
    [[nodiscard]] static BinaryOperator TokenToBinaryOperator(TokenType type);

    /// Converts a unary operator token type into its AST operator enum.
    [[nodiscard]] static UnaryOperator TokenToUnaryOperator(TokenType type);

    /// Parses a CREATE TABLE column type name.
    [[nodiscard]] TypeName ParseTypeName();

private:
    std::vector<Token> tokens_;
    std::size_t current_ = 0;
};

}  // namespace mattsql
