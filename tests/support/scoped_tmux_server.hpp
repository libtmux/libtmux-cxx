#pragma once

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

struct ScopedTmuxServerOptions {
  std::filesystem::path tmux_binary{"tmux"};
  SocketMode mode{SocketMode::Path};
  std::chrono::milliseconds startup_timeout{5000};
  std::chrono::milliseconds teardown_timeout{2000};
  std::string session_name{"libtmux_test"};
  std::shared_ptr<TeardownReport> teardown_report;
};

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
  [[nodiscard]] int server_pid() const noexcept;
  [[nodiscard]] std::vector<std::string> command_prefix() const;
  [[nodiscard]] bool is_alive() const;

private:
  struct State;
  explicit ScopedTmuxServer(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

} // namespace libtmux::test
