#pragma once

#include "libtmux/expected.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace libtmux::test::detail {

using ProcessClock = std::chrono::steady_clock;

struct ReapTicket;

struct ProcessOptions {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::vector<std::string> environment;
  std::size_t capture_limit{64U * 1024U};
};

class ChildProcess final {
public:
  // A successful instance exclusively owns reaping for its direct child. Code
  // outside ChildProcess must not wait for the same child; all normal signals
  // and reaps use the stable pidfd captured immediately after posix_spawnp.
  static libtmux::expected<ChildProcess, std::string> spawn(ProcessOptions options);

  ~ChildProcess() noexcept;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] pid_t pid() const noexcept;
  [[nodiscard]] bool is_running() noexcept;
  [[nodiscard]] bool wait_until(ProcessClock::time_point deadline) noexcept;
  [[nodiscard]] bool send_signal(int signal_number) noexcept;
  void drain_until(ProcessClock::time_point deadline) noexcept;
  void terminate_and_reap(ProcessClock::time_point deadline) noexcept;
  void close_output() noexcept;

  [[nodiscard]] std::optional<int> wait_status() const noexcept;
  [[nodiscard]] const std::string& stdout_text() const noexcept;
  [[nodiscard]] const std::string& stderr_text() const noexcept;

private:
  ChildProcess(pid_t pid, int stdout_fd, int stderr_fd, std::size_t capture_limit,
               std::unique_ptr<ReapTicket> reap_ticket) noexcept;
  void cleanup(ProcessClock::time_point deadline) noexcept;
  void drain_once(ProcessClock::time_point deadline,
                  std::chrono::milliseconds maximum_wait) noexcept;
  void handoff_reap() noexcept;
  void update_status() noexcept;

  pid_t pid_{-1};
  int stdout_fd_{-1};
  int stderr_fd_{-1};
  std::size_t capture_limit_{0};
  std::string stdout_;
  std::string stderr_;
  std::optional<int> wait_status_;
  std::unique_ptr<ReapTicket> reap_ticket_;
  bool owns_child_{false};
};

[[nodiscard]] std::vector<std::string> current_environment();
void erase_environment(std::vector<std::string>& environment, std::string_view name);
void set_environment(std::vector<std::string>& environment, std::string_view name,
                     std::string_view value);

} // namespace libtmux::test::detail
