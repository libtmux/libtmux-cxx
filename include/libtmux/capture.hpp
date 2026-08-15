#pragma once

// Split `capture-pane -p` output into lines.
//
// Every captured line is newline-terminated, so a naive split on '\n' yields a
// final empty element that is not a line. Blank rows inside the pane are
// genuine empty lines and must survive, which is why the terminator is removed
// by dropping exactly one trailing element rather than by trimming empties.
//
// `-p` already strips trailing whitespace from each line; `-N` preserves it.
// Neither is re-implemented here: the caller chooses the flag and this only
// frames what tmux returned.

#include "libtmux/abi.hpp"
#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

// The lines are views into the text, so text that dies at the semicolon takes
// them with it — and `pane.capture()` returns its output by value, which makes
// `capture_lines(*pane.capture())` the natural thing to write and a
// use-after-free to run. Deleted for an rvalue string only: an lvalue string
// converts as before, and so does a literal.
template <typename Text>
  requires std::same_as<std::remove_cvref_t<Text>, std::string> &&
               (!std::is_lvalue_reference_v<Text>)
std::vector<std::string_view> capture_lines(Text&&) = delete;

[[nodiscard]] inline std::vector<std::string_view>
capture_lines(std::string_view output) {
  std::vector<std::string_view> lines;
  if (output.empty()) {
    return lines;
  }
  std::size_t position = 0;
  while (position <= output.size()) {
    const std::size_t end = output.find('\n', position);
    if (end == std::string_view::npos) {
      lines.push_back(output.substr(position));
      break;
    }
    lines.push_back(output.substr(position, end - position));
    position = end + 1;
    // A terminator at the very end closes the last line rather than opening
    // an empty one.
    if (position == output.size()) {
      break;
    }
  }
  return lines;
}

// Drop the blank rows a pane pads its height with, keeping blank lines that
// have content below them.
[[nodiscard]] inline std::vector<std::string_view>
without_trailing_blanks(std::vector<std::string_view> lines) {
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  return lines;
}

LIBTMUX_NAMESPACE_END
