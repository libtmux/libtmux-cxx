#pragma once

// Build tmux target specifiers.
//
// tmux addresses objects either by id (`$0`, `@0`, `%0`) or by the
// `session:window.pane` path. Ids are unambiguous; the path is not, because
// `:` and `.` are its separators and a session or window name containing one
// silently re-parses as a different target. Prefer an id whenever one exists,
// and refuse to build a path from a name that cannot survive it.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"
#include <string>
#include <string_view>

LIBTMUX_NAMESPACE_BEGIN

enum class TargetError {
  empty_name,
  // A name holding a separator would re-parse as a different target.
  separator_in_name,
};

[[nodiscard]] constexpr std::string_view to_string(TargetError error) noexcept {
  switch (error) {
  case TargetError::empty_name:
    return "the name is empty";
  case TargetError::separator_in_name:
    return "the name contains a target separator and would address something else";
  }
  return "unknown target error";
}

[[nodiscard]] constexpr bool is_pane_id(std::string_view value) noexcept {
  return value.size() > 1 && value.front() == '%';
}
[[nodiscard]] constexpr bool is_window_id(std::string_view value) noexcept {
  return value.size() > 1 && value.front() == '@';
}
[[nodiscard]] constexpr bool is_session_id(std::string_view value) noexcept {
  return value.size() > 1 && value.front() == '$';
}

// Validate one path component. Ids skip validation because they contain no
// separator by construction.
[[nodiscard]] inline expected<std::string, TargetError>
path_component(std::string_view name) {
  if (name.empty()) {
    return unexpected(TargetError::empty_name);
  }
  if (name.find_first_of(":.") != std::string_view::npos) {
    return unexpected(TargetError::separator_in_name);
  }
  return std::string{name};
}

// A session target is its id, or its validated name.
[[nodiscard]] inline expected<std::string, TargetError>
session_target(std::string_view session) {
  if (is_session_id(session)) {
    return std::string{session};
  }
  return path_component(session);
}

// A window target is its id, which needs no session, or `session:window`.
[[nodiscard]] inline expected<std::string, TargetError>
window_target(std::string_view session, std::string_view window) {
  if (is_window_id(window)) {
    return std::string{window};
  }
  const auto left = session_target(session);
  if (!left.has_value()) {
    return left;
  }
  const auto right = path_component(window);
  if (!right.has_value()) {
    return right;
  }
  return *left + ":" + *right;
}

// A pane target is its id, or `session:window.pane`.
[[nodiscard]] inline expected<std::string, TargetError>
pane_target(std::string_view session, std::string_view window, std::string_view pane) {
  if (is_pane_id(pane)) {
    return std::string{pane};
  }
  const auto left = window_target(session, window);
  if (!left.has_value()) {
    return left;
  }
  const auto right = path_component(pane);
  if (!right.has_value()) {
    return right;
  }
  return *left + "." + *right;
}

LIBTMUX_NAMESPACE_END
