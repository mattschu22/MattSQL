#pragma once

#include <cstddef>
#include <string>

namespace mattsql {

enum class TokenType {
  // EndOfFile is always emitted as the final token; Invalid preserves the
  // source lexeme for malformed input that the lexer can recover from.
  EndOfFile,
  Invalid,

  // Keywords
  Select,
  From,
  Where,
  Insert,
  Into,
  Values,
  Create,
  Table,
  Drop,
  Null,
  And,
  Or,
  Not,
  As,
  Order,
  By,
  Limit,
  True,
  False,

  // Identifiers and literals
  Identifier,
  Integer,
  String,

  // Operators
  Plus,         // +
  Minus,        // -
  Star,         // *
  Slash,        // /
  Equal,        // =
  NotEqual,     // != or <>
  Less,         // <
  LessEqual,    // <=
  Greater,      // >
  GreaterEqual, // >=

  // Punctuation
  LeftParen,
  RightParen,
  Comma,
  Dot,
  Semicolon
};

struct SourceLocation {
  // Lines and columns are 1-based for diagnostics; offset is 0-based.
  std::size_t line = 1;
  std::size_t column = 1;
  std::size_t offset = 0;
};

struct SourceRange {
  SourceLocation start;
  // End points one character past the final character in the token.
  SourceLocation end;
};

struct Token {
  TokenType type;
  // The exact source text for the token, including original keyword casing.
  std::string lexeme;
  SourceRange range;
};

} // namespace mattsql
