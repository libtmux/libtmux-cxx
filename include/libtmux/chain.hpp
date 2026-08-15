#pragma once

// Compose several tmux commands as one fail-fast group.
//
// A chain is a typed front for a batch: each step validates its own arguments
// as it is added, so a bad target or key name is reported where it was written
// rather than as a tmux message about a command the caller cannot see.
//
// The chain records the first validation failure and stops accumulating. That
// keeps the fluent form honest — the alternative, throwing mid-expression or
// silently dropping a step, both leave the caller guessing which parts ran.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtmux/abi.hpp"
#include "libtmux/batch.hpp"
#include "libtmux/keys.hpp"
#include "libtmux/target.hpp"

LIBTMUX_NAMESPACE_BEGIN

class Chain {
public:
  Chain& new_session(std::string_view name, bool detached = true) {
    const auto target = session_target(name);
    if (!target.has_value()) {
      return fail("session name cannot address itself");
    }
    std::vector<std::string> command{"new-session", "-s", *target};
    if (detached) {
      command.emplace_back("-d");
    }
    return add(std::move(command));
  }

  Chain& new_window(std::string_view session, std::string_view name) {
    const auto target = session_target(session);
    if (!target.has_value()) {
      return fail("session name cannot address itself");
    }
    if (!path_component(name).has_value()) {
      return fail("window name cannot address itself");
    }
    return add({"new-window", "-t", *target, "-n", std::string{name}});
  }

  Chain& split_window(std::string_view session, std::string_view window) {
    const auto target = window_target(session, window);
    if (!target.has_value()) {
      return fail("window target cannot be addressed");
    }
    return add({"split-window", "-t", *target});
  }

  // Literal text, never interpreted as key names or formats.
  Chain& send_text(std::string_view target, std::string_view text) {
    const auto arguments = literal_arguments(text);
    if (!arguments.has_value()) {
      return fail("text to send is empty");
    }
    std::vector<std::string> command{"send-keys", "-t", std::string{target}};
    command.insert(command.end(), arguments->begin(), arguments->end());
    return add(std::move(command));
  }

  Chain& send_key(std::string_view target, std::string_view key) {
    if (!is_key_name(key)) {
      return fail("unknown key name: " + std::string{key});
    }
    return add({"send-keys", "-t", std::string{target}, std::string{key}});
  }

  // Escape hatch for a command the typed steps do not cover.
  Chain& command(std::vector<std::string> argv) { return add(std::move(argv)); }

  [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] const CommandBatch& batch() const noexcept { return batch_; }

private:
  Chain& add(std::vector<std::string> command) {
    if (error_.empty() && !batch_.add(std::move(command))) {
      error_ = "empty command";
    }
    return *this;
  }

  Chain& fail(std::string reason) {
    if (error_.empty()) {
      error_ = std::move(reason);
    }
    return *this;
  }

  CommandBatch batch_;
  std::string error_;
};

LIBTMUX_NAMESPACE_END
