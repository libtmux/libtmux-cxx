#pragma once

// A private tmux server, torn down with the scope that started it.
//
// Exported as `libtmux::testing`, a target separate from the library. The
// server gets a `mkdtemp` tree of its own, `TMUX_TMPDIR` inside it, and a
// child environment with `TMUX` and `TMUX_PANE` erased, so a suite run from
// inside tmux cannot reach the surrounding server. Teardown kills the server
// and removes the tree even when a test aborts.

#include "libtmux/expected.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libtmux::test {

enum class SocketMode { Name, Path };

struct TeardownReport {
  std::vector<std::string> messages;
};

// Names the private tree, the socket and the `tmux -L` name, so concurrent
// suites cannot collide and a leftover directory identifies its owner.
//
// `label` reaches the socket path, which must fit in `sockaddr_un::sun_path`
// — 108 bytes on Linux, 104 on macOS. Keep it under about twenty characters:
// macOS spends roughly sixty of those on `$TMPDIR` alone, once
// `/var/folders/...` has canonicalised to `/private/var/...`, and tmux spends
// more again on the directory it puts the socket in. A label that overruns is
// reported by tmux as "File name too long", naming the path.
//
// `start` refuses one containing anything outside `[A-Za-z0-9._-]`.
struct SocketNamespace {
  std::string label{"libtmux-cxx-test"};

  [[nodiscard]] static SocketNamespace internal() { return {}; }

  // Prefixed, so a consumer's servers are distinguishable from this library's.
  [[nodiscard]] static SocketNamespace consumer(std::string_view suite) {
    return SocketNamespace{"libtmux-cxx-" + std::string{suite}};
  }
};

struct ScopedTmuxServerOptions {
  std::filesystem::path tmux_binary{"tmux"};
  SocketMode mode{SocketMode::Path};
  std::chrono::milliseconds startup_timeout{5000};
  std::chrono::milliseconds teardown_timeout{2000};
  std::string session_name{"libtmux_test"};
  SocketNamespace socket_namespace{};
  // Every member has an initializer: without one here, a designated
  // initializer that stops short of it warns under
  // `-Wmissing-field-initializers`.
  std::shared_ptr<TeardownReport> teardown_report{};
};

// Build and edit a `NAME=VALUE` block for a child process.
// `current_environment` copies this process's; `set_environment` replaces any
// existing entry rather than appending; `erase_environment` removes every
// entry for the name.
[[nodiscard]] std::vector<std::string> current_environment();
void set_environment(std::vector<std::string>& environment, std::string_view name,
                     std::string_view value);
void erase_environment(std::vector<std::string>& environment, std::string_view name);

class ScopedTmuxServer final {
public:
  static libtmux::expected<ScopedTmuxServer, std::string>
  start(ScopedTmuxServerOptions options = {});
  ~ScopedTmuxServer() noexcept;
  ScopedTmuxServer(ScopedTmuxServer&&) noexcept;
  ScopedTmuxServer& operator=(ScopedTmuxServer&&) noexcept;
  ScopedTmuxServer(const ScopedTmuxServer&) = delete;
  ScopedTmuxServer& operator=(const ScopedTmuxServer&) = delete;

  [[nodiscard]] SocketMode socket_mode() const noexcept;
  [[nodiscard]] std::optional<std::string_view> socket_name() const noexcept;
  [[nodiscard]] const std::filesystem::path& socket_path() const noexcept;
  [[nodiscard]] const std::filesystem::path& tmux_tmpdir() const noexcept;
  [[nodiscard]] std::string_view session_name() const noexcept;
  [[nodiscard]] std::string_view socket_namespace() const noexcept;
  [[nodiscard]] int server_pid() const noexcept;
  [[nodiscard]] std::vector<std::string> command_prefix() const;
  [[nodiscard]] bool is_alive() const;

  // `NAME=VALUE` entries for a child that should reach this server and not the
  // surrounding one: `TMUX_TMPDIR` inside the private tree, `TMUX` and
  // `TMUX_PANE` absent.
  [[nodiscard]] std::vector<std::string> child_environment() const;

private:
  struct State;
  explicit ScopedTmuxServer(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

} // namespace libtmux::test
