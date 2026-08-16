#pragma once

// A tmux server that belongs to one test, and dies with it.
//
// This ships. Anyone writing tests *against* this library has the same problem
// its own suite has — a tmux server is shared state keyed only by its socket,
// so two suites that pick the same one delete each other's sessions and
// produce failures that look like bugs in whichever noticed first. Solving
// that correctly means a private `$TMUX_TMPDIR`, a socket under it, an erased
// `TMUX`/`TMUX_PANE` so a suite run from inside tmux cannot reach outward, and
// a teardown that reaps the server even when the test aborts. That is too much
// to ask every consumer to rediscover, and a consumer who gets it wrong takes
// somebody's real session with them.
//
// So it is exported as `libtmux::testing`, separately from the library: a
// program that only *uses* libtmux links none of this.

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

// The name every socket, tree and session this fixture makes is filed under.
//
// One machine runs more than one suite: other language ports of libtmux, other
// checkouts of this one, this library's own tests, and — separately — the
// tests that drive its examples. Each wants servers it can identify as its own
// and clean up without touching a neighbour's. A namespace is how they say
// which is which, and a stray directory in `$TMPDIR` names its owner instead
// of being one more `tmux-1000` nobody dares delete.
//
// It is not decoration. `label` reaches the socket path, and the whole path
// has to fit in `sockaddr_un::sun_path` — 108 bytes on Linux, 104 on macOS —
// so `start` refuses a label that would overrun it rather than letting tmux
// fail with something unreadable. Keep it short and filesystem-safe:
// characters outside `[A-Za-z0-9._-]` are rejected for the same reason.
struct SocketNamespace {
  std::string label{"libtmux-cxx-test"};

  // The house default, kept as a named thing so a consumer can say it meant
  // this one rather than spelling the string again.
  [[nodiscard]] static SocketNamespace internal() { return {}; }

  // For a suite that consumes the installed package — the examples' own tests
  // are the first, and the point of the split: their servers are visibly not
  // this library's.
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
  std::shared_ptr<TeardownReport> teardown_report;
};

// Editing a `NAME=VALUE` block of the kind `child_environment()` returns.
//
// Public because `child_environment()` is: handing a caller a vector of
// strings and leaving them to work out that a name may appear only once, and
// that `TMUX=` is not the same as `TMUX` being absent, would be handing them a
// bug. `set_environment` replaces rather than appends; `erase_environment`
// removes every entry for the name.
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

  // The environment a child needs to address this server as tmux would: an
  // exported `TMUX_TMPDIR` pointing into the private tree, and `TMUX` and
  // `TMUX_PANE` erased so the child cannot reach the server the suite itself
  // is running inside. Entries are `NAME=VALUE`; a name with no `=` means
  // "remove this one".
  //
  // This exists because the examples are programs, not functions: the only way
  // to test one is to run it, and the only way to keep it off a real server is
  // to hand it this.
  [[nodiscard]] std::vector<std::string> child_environment() const;

private:
  struct State;
  explicit ScopedTmuxServer(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

} // namespace libtmux::test
