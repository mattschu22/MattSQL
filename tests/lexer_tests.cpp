#include "mattsql/lexer/lexer.hpp"
#include "mattsql/lexer/token.hpp"

#include "test_framework.hpp"

#include <string>
#include <vector>

namespace {

/// Asserts a token's type, lexeme, and starting source location.
void expect_token(const std::vector<mattsql::Token> &tokens, std::size_t index,
                  mattsql::TokenType type, const std::string &lexeme, std::size_t line,
                  std::size_t column) {
  EXPECT_TRUE(index < tokens.size());
  EXPECT_TRUE(tokens[index].type == type);
  EXPECT_EQ(tokens[index].lexeme, lexeme);
  EXPECT_EQ(tokens[index].range.start.line, line);
  EXPECT_EQ(tokens[index].range.start.column, column);
}

} // namespace

/// Verifies keyword, identifier, and punctuation tokenization.
TEST_CASE(tokenizes_keywords_identifiers_and_punctuation) {
  mattsql::Lexer lexer("SELECT value AS name FROM table_1;");

  const auto tokens = lexer.Tokenize();

  EXPECT_EQ(tokens.size(), 8U);
  expect_token(tokens, 0, mattsql::TokenType::Select, "SELECT", 1U, 1U);
  expect_token(tokens, 1, mattsql::TokenType::Identifier, "value", 1U, 8U);
  expect_token(tokens, 2, mattsql::TokenType::As, "AS", 1U, 14U);
  expect_token(tokens, 3, mattsql::TokenType::Identifier, "name", 1U, 17U);
  expect_token(tokens, 4, mattsql::TokenType::From, "FROM", 1U, 22U);
  expect_token(tokens, 5, mattsql::TokenType::Identifier, "table_1", 1U, 27U);
  expect_token(tokens, 6, mattsql::TokenType::Semicolon, ";", 1U, 34U);
  expect_token(tokens, 7, mattsql::TokenType::EndOfFile, "", 1U, 35U);
}

/// Verifies literal and comparison operator tokenization.
TEST_CASE(tokenizes_literals_and_comparison_operators) {
  mattsql::Lexer lexer("where age >= 21 and name <> 'Matt''SQL';");

  const auto tokens = lexer.Tokenize();

  EXPECT_EQ(tokens.size(), 10U);
  expect_token(tokens, 0, mattsql::TokenType::Where, "where", 1U, 1U);
  expect_token(tokens, 1, mattsql::TokenType::Identifier, "age", 1U, 7U);
  expect_token(tokens, 2, mattsql::TokenType::GreaterEqual, ">=", 1U, 11U);
  expect_token(tokens, 3, mattsql::TokenType::Integer, "21", 1U, 14U);
  expect_token(tokens, 4, mattsql::TokenType::And, "and", 1U, 17U);
  expect_token(tokens, 5, mattsql::TokenType::Identifier, "name", 1U, 21U);
  expect_token(tokens, 6, mattsql::TokenType::NotEqual, "<>", 1U, 26U);
  expect_token(tokens, 7, mattsql::TokenType::String, "'Matt''SQL'", 1U, 29U);
  expect_token(tokens, 8, mattsql::TokenType::Semicolon, ";", 1U, 40U);
  expect_token(tokens, 9, mattsql::TokenType::EndOfFile, "", 1U, 41U);
}

/// Verifies TRUE and FALSE are keywords without stealing longer identifiers.
TEST_CASE(tokenizes_boolean_keywords_and_identifier_prefixes) {
  mattsql::Lexer lexer("TRUE false true_value falsehood");

  const auto tokens = lexer.Tokenize();

  EXPECT_EQ(tokens.size(), 5U);
  expect_token(tokens, 0, mattsql::TokenType::True, "TRUE", 1U, 1U);
  expect_token(tokens, 1, mattsql::TokenType::False, "false", 1U, 6U);
  expect_token(tokens, 2, mattsql::TokenType::Identifier, "true_value", 1U, 12U);
  expect_token(tokens, 3, mattsql::TokenType::Identifier, "falsehood", 1U, 23U);
  expect_token(tokens, 4, mattsql::TokenType::EndOfFile, "", 1U, 32U);
}

/// Verifies comments are skipped while multiline locations remain correct.
TEST_CASE(skips_comments_and_tracks_multiline_locations) {
  mattsql::Lexer lexer("-- ignored\n/* also ignored */\nSELECT\n  42");

  const auto tokens = lexer.Tokenize();

  EXPECT_EQ(tokens.size(), 3U);
  expect_token(tokens, 0, mattsql::TokenType::Select, "SELECT", 3U, 1U);
  expect_token(tokens, 1, mattsql::TokenType::Integer, "42", 4U, 3U);
  expect_token(tokens, 2, mattsql::TokenType::EndOfFile, "", 4U, 5U);
}

/// Verifies malformed lexemes are emitted as invalid tokens.
TEST_CASE(reports_invalid_characters_and_unclosed_strings) {
  mattsql::Lexer lexer("SELECT @, 'open");

  const auto tokens = lexer.Tokenize();

  EXPECT_EQ(tokens.size(), 5U);
  expect_token(tokens, 0, mattsql::TokenType::Select, "SELECT", 1U, 1U);
  expect_token(tokens, 1, mattsql::TokenType::Invalid, "@", 1U, 8U);
  expect_token(tokens, 2, mattsql::TokenType::Comma, ",", 1U, 9U);
  expect_token(tokens, 3, mattsql::TokenType::Invalid, "'open", 1U, 11U);
  expect_token(tokens, 4, mattsql::TokenType::EndOfFile, "", 1U, 16U);
}
