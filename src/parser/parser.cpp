#include "mattsql/parser/parser.hpp"

#include "mattsql/common/identifier.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mattsql {
namespace {

/// Decodes a quoted SQL string token into its AST literal value.
[[nodiscard]] std::string unescape_sql_string(std::string_view lexeme) {
  if (lexeme.size() < 2 || lexeme.front() != '\'' || lexeme.back() != '\'') {
    throw std::runtime_error("malformed string token");
  }

  std::string value;
  value.reserve(lexeme.size() - 2);

  // The lexer keeps string lexemes quoted. SQL represents an embedded single
  // quote as two adjacent quote characters, so the AST stores the decoded value.
  for (std::size_t index = 1; index + 1 < lexeme.size(); ++index) {
    if (lexeme[index] == '\'' && index + 1 < lexeme.size() - 1 &&
        lexeme[index + 1] == '\'') {
      value.push_back('\'');
      ++index;
      continue;
    }

    value.push_back(lexeme[index]);
  }

  return value;
}

} // namespace

/// Creates a parser and appends an EOF sentinel when the token stream lacks one.
Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
  if (tokens_.empty()) {
    tokens_.push_back(Token{TokenType::EndOfFile, "", SourceRange{}});
    return;
  }

  if (tokens_.back().type != TokenType::EndOfFile) {
    const auto location = tokens_.back().range.end;
    tokens_.push_back(Token{TokenType::EndOfFile, "", SourceRange{location, location}});
  }
}

/// Parses one complete SQL statement and verifies there is no trailing input.
StatementPtr Parser::ParseStatement() {
  StatementPtr statement;

  if (Match(TokenType::Select)) {
    statement = ParseSelectStatement();
  } else if (Match(TokenType::Insert)) {
    statement = ParseInsertStatement();
  } else if (Match(TokenType::Create)) {
    statement = ParseCreateTableStatement();
  } else if (Match(TokenType::Invalid)) {
    throw ParseError("invalid token: " + Previous().lexeme, Previous().range.start);
  } else {
    throw ParseError("expected SQL statement", Peek().range.start);
  }

  if (Check(TokenType::Semicolon)) {
    Advance();
  }

  if (!IsAtEnd()) {
    throw ParseError("expected end of statement", Peek().range.start);
  }

  return statement;
}

/// Returns true when the current token is EOF.
bool Parser::IsAtEnd() const { return Peek().type == TokenType::EndOfFile; }

/// Returns the current token without consuming it.
const Token &Parser::Peek() const { return tokens_[current_]; }

/// Returns the token most recently consumed by Advance.
const Token &Parser::Previous() const { return tokens_[current_ - 1]; }

/// Consumes the current token unless it is EOF, then returns the previous token.
const Token &Parser::Advance() {
  if (!IsAtEnd()) {
    ++current_;
  }

  return Previous();
}

/// Returns true when the current token matches the requested type.
bool Parser::Check(TokenType type) const { return Peek().type == type; }

/// Consumes and returns true when the current token matches the requested type.
bool Parser::Match(TokenType type) {
  if (!Check(type)) {
    return false;
  }

  Advance();
  return true;
}

/// Consumes a required token or throws a located parse error.
const Token &Parser::Consume(TokenType type, const std::string &message) {
  if (Check(type)) {
    return Advance();
  }

  throw ParseError(message, Peek().range.start);
}

/// Parses a SELECT statement after consuming its leading SELECT token.
StatementPtr Parser::ParseSelectStatement() {
  auto statement = std::make_unique<SelectStatement>();
  statement->projections = ParseSelectProjectionList();

  if (Match(TokenType::From)) {
    statement->table_name =
        Consume(TokenType::Identifier, "expected table name after FROM").lexeme;
  }

  if (Match(TokenType::Where)) {
    statement->where = ParseExpression();
  }

  return statement;
}

/// Parses an INSERT INTO statement after consuming its leading INSERT token.
StatementPtr Parser::ParseInsertStatement() {
  auto statement = std::make_unique<InsertStatement>();

  Consume(TokenType::Into, "expected INTO after INSERT");
  statement->table_name =
      Consume(TokenType::Identifier, "expected table name after INTO").lexeme;
  Consume(TokenType::Values, "expected VALUES after INSERT table name");
  Consume(TokenType::LeftParen, "expected '(' before VALUES list");
  statement->values = ParseExpressionList();
  Consume(TokenType::RightParen, "expected ')' after VALUES list");

  return statement;
}

/// Parses a CREATE TABLE statement after consuming its leading CREATE token.
StatementPtr Parser::ParseCreateTableStatement() {
  auto statement = std::make_unique<CreateTableStatement>();

  Consume(TokenType::Table, "expected TABLE after CREATE");
  statement->table_name =
      Consume(TokenType::Identifier, "expected table name after CREATE TABLE").lexeme;
  Consume(TokenType::LeftParen, "expected '(' before column definitions");

  do {
    ColumnDefinition column;
    column.name = Consume(TokenType::Identifier, "expected column name").lexeme;
    column.type = ParseTypeName();
    statement->columns.push_back(std::move(column));
  } while (Match(TokenType::Comma));

  Consume(TokenType::RightParen, "expected ')' after column definitions");
  return statement;
}

/// Parses one or more comma-separated expressions.
std::vector<ExpressionPtr> Parser::ParseExpressionList() {
  std::vector<ExpressionPtr> expressions;
  expressions.push_back(ParseExpression());

  while (Match(TokenType::Comma)) {
    expressions.push_back(ParseExpression());
  }

  return expressions;
}

/// Parses SELECT projections and optional AS aliases.
std::vector<SelectProjection> Parser::ParseSelectProjectionList() {
  std::vector<SelectProjection> projections;

  do {
    SelectProjection projection;
    projection.expression = ParseExpression();

    if (Match(TokenType::As)) {
      projection.alias =
          Consume(TokenType::Identifier, "expected alias after AS").lexeme;
    }

    projections.push_back(std::move(projection));
  } while (Match(TokenType::Comma));

  return projections;
}

/// Parses an expression with precedence climbing from the minimum precedence.
ExpressionPtr Parser::ParseExpression(int min_precedence) {
  auto left = ParsePrefixExpression();

  while (true) {
    const auto precedence = GetBinaryPrecedence(Peek().type);
    if (precedence < min_precedence) {
      break;
    }

    const auto operator_token = Advance();
    auto right = ParseExpression(precedence + 1);

    left = std::make_unique<BinaryExpression>(
        std::move(left), TokenToBinaryOperator(operator_token.type), std::move(right));
  }

  return left;
}

/// Parses prefix unary operators before delegating to primary expressions.
ExpressionPtr Parser::ParsePrefixExpression() {
  if (Match(TokenType::Plus) || Match(TokenType::Minus) || Match(TokenType::Not)) {
    const auto operator_type = Previous().type;

    // Parse the operand with prefix parsing again so chained prefixes like
    // NOT -flag bind before any following binary operator.
    return std::make_unique<UnaryExpression>(TokenToUnaryOperator(operator_type),
                                             ParsePrefixExpression());
  }

  return ParsePrimaryExpression();
}

/// Parses literals, column references, star, or parenthesized expressions.
ExpressionPtr Parser::ParsePrimaryExpression() {
  if (Match(TokenType::Integer)) {
    try {
      return std::make_unique<IntegerLiteral>(std::stoll(Previous().lexeme));
    } catch (const std::exception &) {
      throw ParseError("integer literal is out of range", Previous().range.start);
    }
  }

  if (Match(TokenType::String)) {
    try {
      return std::make_unique<StringLiteral>(unescape_sql_string(Previous().lexeme));
    } catch (const std::exception &) {
      throw ParseError("malformed string literal", Previous().range.start);
    }
  }

  if (Match(TokenType::True)) {
    return std::make_unique<BooleanLiteral>(true);
  }

  if (Match(TokenType::False)) {
    return std::make_unique<BooleanLiteral>(false);
  }

  if (Match(TokenType::Null)) {
    return std::make_unique<NullLiteral>();
  }

  if (Match(TokenType::Star)) {
    return std::make_unique<StarExpression>();
  }

  if (Match(TokenType::Identifier)) {
    std::string name = Previous().lexeme;

    // Keep dotted names as a single ColumnRef until semantic analysis resolves
    // relation and column components.
    while (Match(TokenType::Dot)) {
      name += ".";
      name += Consume(TokenType::Identifier, "expected identifier after '.'").lexeme;
    }

    return std::make_unique<ColumnRef>(std::move(name));
  }

  if (Match(TokenType::LeftParen)) {
    auto expression = ParseExpression();
    Consume(TokenType::RightParen, "expected ')' after expression");
    return expression;
  }

  if (Match(TokenType::Invalid)) {
    throw ParseError("invalid token: " + Previous().lexeme, Previous().range.start);
  }

  throw ParseError("expected expression", Peek().range.start);
}

/// Returns a binary operator precedence or -1 for non-binary tokens.
int Parser::GetBinaryPrecedence(TokenType type) {
  switch (type) {
  case TokenType::Or:
    return 1;
  case TokenType::And:
    return 2;
  case TokenType::Equal:
  case TokenType::NotEqual:
  case TokenType::Less:
  case TokenType::LessEqual:
  case TokenType::Greater:
  case TokenType::GreaterEqual:
    return 3;
  case TokenType::Plus:
  case TokenType::Minus:
    return 4;
  case TokenType::Star:
  case TokenType::Slash:
    return 5;
  default:
    return -1;
  }
}

/// Converts a binary operator token type into its AST operator enum.
BinaryOperator Parser::TokenToBinaryOperator(TokenType type) {
  switch (type) {
  case TokenType::Plus:
    return BinaryOperator::Add;
  case TokenType::Minus:
    return BinaryOperator::Subtract;
  case TokenType::Star:
    return BinaryOperator::Multiply;
  case TokenType::Slash:
    return BinaryOperator::Divide;
  case TokenType::Equal:
    return BinaryOperator::Equal;
  case TokenType::NotEqual:
    return BinaryOperator::NotEqual;
  case TokenType::Less:
    return BinaryOperator::Less;
  case TokenType::LessEqual:
    return BinaryOperator::LessEqual;
  case TokenType::Greater:
    return BinaryOperator::Greater;
  case TokenType::GreaterEqual:
    return BinaryOperator::GreaterEqual;
  case TokenType::And:
    return BinaryOperator::And;
  case TokenType::Or:
    return BinaryOperator::Or;
  default:
    throw std::logic_error("token is not a binary operator");
  }
}

/// Converts a unary operator token type into its AST operator enum.
UnaryOperator Parser::TokenToUnaryOperator(TokenType type) {
  switch (type) {
  case TokenType::Plus:
    return UnaryOperator::Plus;
  case TokenType::Minus:
    return UnaryOperator::Minus;
  case TokenType::Not:
    return UnaryOperator::Not;
  default:
    throw std::logic_error("token is not a unary operator");
  }
}

/// Parses a supported column type name for CREATE TABLE.
TypeName Parser::ParseTypeName() {
  const auto &token = Consume(TokenType::Identifier, "expected column type");
  const auto type_name = FoldIdentifierKey(token.lexeme);

  if (type_name == "int" || type_name == "integer") {
    return TypeName::Int;
  }
  if (type_name == "text") {
    return TypeName::Text;
  }
  if (type_name == "bool" || type_name == "boolean") {
    return TypeName::Bool;
  }

  throw ParseError("unknown column type: " + token.lexeme, token.range.start);
}

} // namespace mattsql
