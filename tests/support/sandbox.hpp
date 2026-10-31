#pragma once

// What a test process may assume about the tmux it can reach.
//
// ctest gives every test a clean environment (cmake/LibtmuxTestSandbox.cmake):
// nothing that selects a server, and `TMUX_TMPDIR` pointing at a directory
// made for the build, named again in `LIBTMUX_TEST_SANDBOX`. A process that
// finds anything else could send a bare `tmux` command to the server its
// caller is inside, or to the socket directory its caller's own sessions live
// in. The shared `main` refuses to run in that case.

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace libtmux::test_support {

/// Variables that choose which tmux server a command reaches.
inline constexpr std::array<std::string_view, 4> server_selectors{
    "TMUX", "TMUX_PANE", "LIBTMUX_SOCKET", "LIBTMUX_SOCKET_PATH"};

/// The longest default socket path the suite accepts: a unix socket address
/// holds 104 bytes on macOS and 108 on Linux.
inline constexpr std::size_t socket_path_budget = 100;

/// The variable that vouches for `TMUX_TMPDIR`: the directory the harness made.
inline constexpr std::string_view sandbox_marker = "LIBTMUX_TEST_SANDBOX";

/// What keeps this process from being sandboxed, one finding each; empty when
/// nothing does. A finding names the variable and never quotes its value: the
/// refusal goes to a terminal and a log, and `TMUX` and the paths hold session
/// and directory names.
inline std::vector<std::string> sandbox_violations() {
  std::vector<std::string> found;
  for (const std::string_view name : server_selectors) {
    const std::string key{name};
    if (const char* const value = std::getenv(key.c_str()); value != nullptr) {
      found.push_back(key + " is set");
    }
  }

  const char* const tmpdir = std::getenv("TMUX_TMPDIR");
  if (tmpdir == nullptr || *tmpdir == '\0') {
    found.emplace_back("TMUX_TMPDIR is not set, so tmux uses the shared /tmp");
    return found;
  }
  std::error_code failed;
  const auto resolved = std::filesystem::canonical(tmpdir, failed);
  if (failed) {
    found.push_back("TMUX_TMPDIR is not a directory: " + failed.message());
    return found;
  }

  const std::string marker_name{sandbox_marker};
  const char* const marker = std::getenv(marker_name.c_str());
  if (marker == nullptr || *marker == '\0') {
    found.push_back("TMUX_TMPDIR is not vouched for: " + marker_name + " is not set");
  } else {
    const auto named = std::filesystem::canonical(marker, failed);
    if (failed || named != resolved) {
      found.push_back("TMUX_TMPDIR is not the directory " + marker_name + " names");
    }
  }

  struct stat metadata = {};
  if (::stat(resolved.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
      metadata.st_uid != ::geteuid() || (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    found.push_back("TMUX_TMPDIR is not a directory only this user can use");
  }

  // tmux puts its socket at `tmux-<uid>/default` below it, and the kernel
  // refuses a longer address with a message that names nothing else.
  const auto socket = resolved / ("tmux-" + std::to_string(::getuid())) / "default";
  if (socket.string().size() > socket_path_budget) {
    found.push_back("the default socket path under TMUX_TMPDIR is longer than " +
                    std::to_string(socket_path_budget) + " bytes");
  }
  return found;
}

/// Removes a variable for a scope and puts back what was there. The inverse of
/// `libtmux::test::EnvironmentGuard`, which can only set one.
class ScopedUnset final {
public:
  explicit ScopedUnset(std::string name) : name_{std::move(name)} {
    if (const char* const existing = std::getenv(name_.c_str()); existing != nullptr) {
      previous_ = existing;
    }
    ::unsetenv(name_.c_str());
  }
  ~ScopedUnset() {
    if (previous_.has_value()) {
      ::setenv(name_.c_str(), previous_->c_str(), 1);
    }
  }
  ScopedUnset(const ScopedUnset&) = delete;
  ScopedUnset& operator=(const ScopedUnset&) = delete;
  ScopedUnset(ScopedUnset&&) = delete;
  ScopedUnset& operator=(ScopedUnset&&) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

/// Ends the process with status 78 unless it is sandboxed, naming each finding.
inline void require_sandbox() {
  const auto found = sandbox_violations();
  if (found.empty()) {
    return;
  }
  std::string message =
      "libtmux tests: refusing to run, this process could reach a tmux server "
      "that is not the suite's\n";
  for (const auto& finding : found) {
    message += "  " + finding + "\n";
  }
  message +=
      "run the suite through ctest, which prepares the environment, or clear "
      "TMUX and set TMUX_TMPDIR to a private directory that LIBTMUX_TEST_SANDBOX "
      "also names\n";
  // The environment may be what is broken, so say it before anything else runs.
  static_cast<void>(::write(STDERR_FILENO, message.data(), message.size()));
  ::_exit(78);
}

} // namespace libtmux::test_support
