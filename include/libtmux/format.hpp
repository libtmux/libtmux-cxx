#pragma once

// Compose tmux format strings.
//
// tmux treats `#` as the start of a substitution: `#{...}` expands a variable,
// `#(...)` runs a command, and `##` is a literal `#`. Any literal text placed
// in a format must therefore escape its `#` characters, or a status line
// containing `#1` silently becomes an expansion attempt.

#include "libtmux/abi.hpp"
#include <string>
#include <string_view>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

/// Escape literal text for inclusion in a format string.
[[nodiscard]] inline std::string escape_literal(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char character : text) {
    if (character == '#') {
      escaped.push_back('#');
    }
    escaped.push_back(character);
  }
  return escaped;
}

/// Wrap a variable name as a substitution. The name is not escaped: a name is
/// chosen by the caller from tmux's format vocabulary, not built from data.
[[nodiscard]] inline std::string variable(std::string_view name) {
  return "#{" + std::string{name} + "}";
}

/// Build a format from alternating literal and variable pieces so a caller
/// never hand-concatenates and forgets to escape one of them.
class FormatBuilder {
public:
  FormatBuilder& literal(std::string_view text) {
    format_ += escape_literal(text);
    return *this;
  }

  FormatBuilder& field(std::string_view name) {
    format_ += variable(name);
    return *this;
  }

  [[nodiscard]] const std::string& str() const noexcept { return format_; }

private:
  std::string format_;
};

LIBTMUX_NAMESPACE_END
