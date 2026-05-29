#pragma once

#include "mattsql/lexer/token.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace mattsql {

class ParseError : public std::runtime_error {
public:
    /// Creates a parse error with a diagnostic message and source location.
    ParseError(std::string message, SourceLocation location)
        : std::runtime_error(message), message_(std::move(message)),
          location_(location) {}

    /// Returns the parser diagnostic without std::exception formatting.
    [[nodiscard]] const std::string& Message() const {
        return message_;
    }

    /// Returns where parsing first determined the input was invalid.
    [[nodiscard]] SourceLocation Location() const {
        return location_;
    }

private:
    std::string message_;
    SourceLocation location_;
};

} // namespace mattsql
