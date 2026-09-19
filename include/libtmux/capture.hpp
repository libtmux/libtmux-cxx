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

namespace detail {

[[nodiscard]] constexpr bool is_word_byte(unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_';
}

// ASCII word boundaries preserve a short answer inside a longer output word.
[[nodiscard]] inline std::string mask_whole_occurrences(std::string_view text,
                                                        std::string_view echo) {
  if (echo.empty() || text.empty()) {
    return std::string{text};
  }
  std::string result;
  std::size_t cursor = 0;
  std::size_t from = 0;
  for (;;) {
    const std::size_t at = text.find(echo, from);
    if (at == std::string_view::npos) {
      break;
    }
    const std::size_t end = at + echo.size();
    const bool opens = at == 0 ||
                       !is_word_byte(static_cast<unsigned char>(text[at - 1])) ||
                       !is_word_byte(static_cast<unsigned char>(echo.front()));
    const bool closes = end == text.size() ||
                        !is_word_byte(static_cast<unsigned char>(text[end])) ||
                        !is_word_byte(static_cast<unsigned char>(echo.back()));
    if (opens && closes) {
      result.append(text.substr(cursor, at - cursor));
      cursor = end;
      from = end;
    } else {
      from = at + 1;
    }
  }
  result.append(text.substr(cursor));
  return result;
}

} // namespace detail

// Whether `wanted` appears in captured text as something the pane produced,
// rather than as text that is merely on screen.
//
// Waiting for a pane to say something is the first thing a supervising program
// needs and the easiest to get wrong, because a capture shows two things that
// are not output. The first is a command still sitting on the prompt: it has
// been typed, nothing has run it, and searching for it succeeds immediately.
// The second is its echo. A shell echoes typed input at least once — the
// kernel's own cooked-mode echo — and often twice more before anything runs,
// from the line editor's redisplay and from any unrelated repaint. None of
// those are output, however many rows they end up spread across.
//
// `sent` is what the calling program itself typed into this pane and has not
// had confirmed. Every whole occurrence of every entry is masked before
// `wanted` is looked for, so an echo cannot be credited to the pane no matter
// where a redraw moved it, and a short entry cannot corrupt a longer real
// word that merely contains it. That is the check row position alone misses:
// the same unsubmitted line, unchanged, after something else pushed it off
// the last row without the pane having produced anything.
//
[[nodiscard]] inline bool output_confirms(std::string_view captured,
                                          std::string_view wanted,
                                          const std::vector<std::string>& sent = {}) {
  if (wanted.empty()) {
    return false;
  }
  // A match confined to the row the cursor is on is a command waiting to run.
  std::string_view settled = captured;
  while (!settled.empty() && settled.back() == '\n') {
    settled.remove_suffix(1);
  }
  const auto last_newline = settled.find_last_of('\n');
  const std::string_view active_row = last_newline == std::string_view::npos
                                          ? settled
                                          : settled.substr(last_newline + 1);
  const std::string_view above = last_newline == std::string_view::npos
                                     ? std::string_view{}
                                     : settled.substr(0, last_newline);
  if (active_row.find(wanted) != std::string_view::npos &&
      above.find(wanted) == std::string_view::npos) {
    return false;
  }

  std::string masked{captured};
  for (const std::string& entry : sent) {
    masked = detail::mask_whole_occurrences(masked, entry);
  }
  return masked.find(wanted) != std::string::npos;
}

LIBTMUX_NAMESPACE_END
