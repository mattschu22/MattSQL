#pragma once

#include "mattsql/lexer/token.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mattsql {

class Lexer {
public:
    /// Creates a lexer over a SQL input view owned by the caller.
    explicit Lexer(std::string_view input);

    /// Returns every token in source order, including the final EOF token.
    [[nodiscard]] std::vector<Token> Tokenize();

private:
    /// Returns true when the cursor has consumed all input characters.
    [[nodiscard]] bool IsAtEnd() const;

    /// Returns the current character without advancing, or '\0' at EOF.
    [[nodiscard]] char Peek() const;

    /// Returns the character after the cursor, or '\0' when unavailable.
    [[nodiscard]] char PeekNext() const;

    /// Consumes the current character only when it matches the expected value.
    [[nodiscard]] bool Match(char expected);

    /// Consumes one character and advances the tracked source location.
    char Advance();

    /// Captures the current cursor and location as the start of the next token.
    void MarkTokenStart();

    /// Skips whitespace and SQL comments while preserving source locations.
    void SkipTrivia();

    /// Builds a token from the current source slice.
    [[nodiscard]] Token MakeToken(TokenType type) const;

    /// Builds a token with an explicit lexeme and the current source range.
    [[nodiscard]] Token MakeToken(TokenType type, std::string lexeme) const;

    /// Dispatches scanning for the next token based on the current character.
    [[nodiscard]] Token ScanToken();

    /// Scans an identifier and upgrades it to a keyword token when applicable.
    [[nodiscard]] Token ScanIdentifierOrKeyword();

    /// Scans an integer literal token.
    [[nodiscard]] Token ScanNumber();

    /// Scans a single-quoted SQL string token.
    [[nodiscard]] Token ScanString();

    /// Scans operators and punctuation tokens.
    [[nodiscard]] Token ScanOperatorOrPunctuation();

    /// Returns true for identifier-start characters.
    [[nodiscard]] static bool IsAlpha(char c);

    /// Returns true for ASCII decimal digits.
    [[nodiscard]] static bool IsDigit(char c);

    /// Returns true for identifier-continuation characters.
    [[nodiscard]] static bool IsAlphaNumeric(char c);

    /// Resolves a case-insensitive keyword or returns Identifier.
    [[nodiscard]] static TokenType LookupKeyword(std::string_view text);

private:
    std::string_view input_;

    std::size_t current_ = 0;
    SourceLocation location_{};

    // Token start state is captured after trivia is skipped and before scanning
    // each token, so token ranges exclude whitespace and comments.
    std::size_t start_ = 0;
    SourceLocation start_location_{};
};

} // namespace mattsql
