#include "mattsql/lexer/lexer.hpp"
#include "mattsql/lexer/token.hpp"

#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace mattsql {

/// Creates a lexer over a caller-owned SQL input view.
Lexer::Lexer(std::string_view input) : input_(input) {}

/// Tokenizes the full SQL input and appends a final EOF token.
std::vector<Token> Lexer::Tokenize() {
  std::vector<Token> tokens;

  while (true) {
    // Trivia is not emitted as tokens, but it still advances source locations.
    SkipTrivia();
    MarkTokenStart();

    auto token = ScanToken();
    const auto token_type = token.type;
    tokens.push_back(std::move(token));

    if (token_type == TokenType::EndOfFile) {
      break;
    }
  }

  return tokens;
}

/// Returns true once the cursor has reached the end of the input.
bool Lexer::IsAtEnd() const { return current_ >= input_.size(); }

/// Returns the current input character without advancing the cursor.
char Lexer::Peek() const {
  if (IsAtEnd()) {
    return '\0';
  }

  return input_[current_];
}

/// Returns the next input character without advancing the cursor.
char Lexer::PeekNext() const {
  if (current_ + 1 >= input_.size()) {
    return '\0';
  }

  return input_[current_ + 1];
}

/// Consumes the current character when it matches the expected character.
bool Lexer::Match(char expected) {
  if (IsAtEnd() || input_[current_] != expected) {
    return false;
  }

  Advance();
  return true;
}

/// Consumes one input character and updates offset, line, and column state.
char Lexer::Advance() {
  const auto character = input_[current_++];
  ++location_.offset;

  // Token ranges use an exclusive end location, so the location is advanced as
  // soon as the character is consumed.
  if (character == '\n') {
    ++location_.line;
    location_.column = 1;
  } else {
    ++location_.column;
  }

  return character;
}

/// Records the current cursor and location as the next token's start.
void Lexer::MarkTokenStart() {
  start_ = current_;
  start_location_ = location_;
}

/// Skips whitespace, line comments, and block comments.
void Lexer::SkipTrivia() {
  while (!IsAtEnd()) {
    const auto character = Peek();

    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      Advance();
      continue;
    }

    if (character == '-' && PeekNext() == '-') {
      Advance();
      Advance();

      // SQL line comments run until, but do not consume, the newline.
      while (!IsAtEnd() && Peek() != '\n') {
        Advance();
      }
      continue;
    }

    if (character == '/' && PeekNext() == '*') {
      Advance();
      Advance();

      bool terminated = false;
      while (!IsAtEnd()) {
        // Block comments may span lines; Advance keeps line and column state in
        // sync while the comment body is skipped.
        if (Peek() == '*' && PeekNext() == '/') {
          Advance();
          Advance();
          terminated = true;
          break;
        }

        Advance();
      }

      if (!terminated) {
        throw std::runtime_error("unterminated block comment");
      }

      continue;
    }

    return;
  }
}

/// Creates a token using the source slice between the token start and cursor.
Token Lexer::MakeToken(TokenType type) const {
  // Most tokens keep their exact source spelling as the lexeme.
  return MakeToken(type, std::string(input_.substr(start_, current_ - start_)));
}

/// Creates a token using an explicit lexeme and the active source range.
Token Lexer::MakeToken(TokenType type, std::string lexeme) const {
  return Token{type, std::move(lexeme), SourceRange{start_location_, location_}};
}

/// Scans the next token from the current cursor position.
Token Lexer::ScanToken() {
  if (IsAtEnd()) {
    return MakeToken(TokenType::EndOfFile, "");
  }

  if (IsAlpha(Peek())) {
    return ScanIdentifierOrKeyword();
  }

  if (IsDigit(Peek())) {
    return ScanNumber();
  }

  if (Peek() == '\'') {
    return ScanString();
  }

  return ScanOperatorOrPunctuation();
}

/// Scans an identifier and returns a keyword token when the spelling matches.
Token Lexer::ScanIdentifierOrKeyword() {
  while (IsAlphaNumeric(Peek())) {
    Advance();
  }

  const auto text = input_.substr(start_, current_ - start_);
  return MakeToken(LookupKeyword(text));
}

/// Scans a contiguous run of decimal digits as an integer token.
Token Lexer::ScanNumber() {
  while (IsDigit(Peek())) {
    Advance();
  }

  return MakeToken(TokenType::Integer);
}

/// Scans a single-quoted SQL string, including doubled quote escapes.
Token Lexer::ScanString() {
  Advance();

  while (!IsAtEnd()) {
    const auto character = Advance();

    if (character != '\'') {
      continue;
    }

    if (Peek() == '\'') {
      // SQL escapes a single quote inside a string by doubling it.
      Advance();
      continue;
    }

    return MakeToken(TokenType::String);
  }

  return MakeToken(TokenType::Invalid);
}

/// Scans a SQL operator or punctuation token from the current character.
Token Lexer::ScanOperatorOrPunctuation() {
  const auto character = Advance();

  switch (character) {
  case '+':
    return MakeToken(TokenType::Plus);
  case '-':
    return MakeToken(TokenType::Minus);
  case '*':
    return MakeToken(TokenType::Star);
  case '/':
    return MakeToken(TokenType::Slash);
  case '=':
    return MakeToken(TokenType::Equal);
  case '!':
    // Bare ! is not a SQL operator in this lexer, but != is accepted.
    if (Match('=')) {
      return MakeToken(TokenType::NotEqual);
    }
    break;
  case '<':
    if (Match('=')) {
      return MakeToken(TokenType::LessEqual);
    }
    if (Match('>')) {
      return MakeToken(TokenType::NotEqual);
    }
    return MakeToken(TokenType::Less);
  case '>':
    if (Match('=')) {
      return MakeToken(TokenType::GreaterEqual);
    }
    return MakeToken(TokenType::Greater);
  case '(':
    return MakeToken(TokenType::LeftParen);
  case ')':
    return MakeToken(TokenType::RightParen);
  case ',':
    return MakeToken(TokenType::Comma);
  case '.':
    return MakeToken(TokenType::Dot);
  case ';':
    return MakeToken(TokenType::Semicolon);
  default:
    break;
  }

  return MakeToken(TokenType::Invalid);
}

/// Returns true for characters that may begin an identifier.
bool Lexer::IsAlpha(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// Returns true for ASCII decimal digit characters.
bool Lexer::IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

/// Returns true for characters that may continue an identifier.
bool Lexer::IsAlphaNumeric(char c) { return IsAlpha(c) || IsDigit(c); }

/// Maps a case-insensitive identifier spelling to a keyword token type.
TokenType Lexer::LookupKeyword(std::string_view text) {
  // Keyword recognition is case-insensitive, while token lexemes retain the
  // original casing from the input.
  std::string lowered;
  lowered.reserve(text.size());

  for (const auto character : text) {
    lowered.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }

  if (lowered == "select") {
    return TokenType::Select;
  }
  if (lowered == "from") {
    return TokenType::From;
  }
  if (lowered == "where") {
    return TokenType::Where;
  }
  if (lowered == "insert") {
    return TokenType::Insert;
  }
  if (lowered == "into") {
    return TokenType::Into;
  }
  if (lowered == "values") {
    return TokenType::Values;
  }
  if (lowered == "create") {
    return TokenType::Create;
  }
  if (lowered == "table") {
    return TokenType::Table;
  }
  if (lowered == "drop") {
    return TokenType::Drop;
  }
  if (lowered == "null") {
    return TokenType::Null;
  }
  if (lowered == "and") {
    return TokenType::And;
  }
  if (lowered == "or") {
    return TokenType::Or;
  }
  if (lowered == "not") {
    return TokenType::Not;
  }
  if (lowered == "as") {
    return TokenType::As;
  }
  if (lowered == "order") {
    return TokenType::Order;
  }
  if (lowered == "by") {
    return TokenType::By;
  }
  if (lowered == "limit") {
    return TokenType::Limit;
  }
  if (lowered == "true") {
    return TokenType::True;
  }
  if (lowered == "false") {
    return TokenType::False;
  }

  return TokenType::Identifier;
}

} // namespace mattsql
