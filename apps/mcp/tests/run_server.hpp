#pragma once

// Run the MCP server against a socket, write a script to its stdin, and
// collect what it wrote back.
//
// Specific rather than general on purpose: the server reads stdin to end and
// answers on stdout, so this needs one write, one read, and a reap. The
// examples' suite has its own runner for a different shape, and neither is
// worth turning into a shared subprocess library.

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libtmux/expected.hpp>

namespace libtmux::mcp::test {

namespace detail {

inline std::vector<char*> writable(std::vector<std::string>& entries) {
  std::vector<char*> pointers;
  pointers.reserve(entries.size() + 1U);
  for (auto& entry : entries) {
    pointers.push_back(entry.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

inline libtmux::expected<void, std::string> write_all(int descriptor,
                                                      std::string_view text) {
  while (!text.empty()) {
    const auto written = ::write(descriptor, text.data(), text.size());
    if (written <= 0) {
      return libtmux::unexpected(std::string{"write: "} + std::strerror(errno));
    }
    text.remove_prefix(static_cast<std::size_t>(written));
  }
  return {};
}

inline libtmux::expected<std::string, std::string>
read_line(int descriptor, std::string& pending,
          std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    if (const auto newline = pending.find('\n'); newline != std::string::npos) {
      std::string line = pending.substr(0, newline);
      pending.erase(0, newline + 1U);
      return line;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      return libtmux::unexpected(std::string{"timed out reading server output"});
    }
    pollfd waiting{.fd = descriptor, .events = POLLIN, .revents = 0};
    if (::poll(&waiting, 1, static_cast<int>(remaining.count())) <= 0) {
      continue;
    }
    std::array<char, 8192> buffer{};
    const auto size = ::read(descriptor, buffer.data(), buffer.size());
    if (size <= 0) {
      return libtmux::unexpected(std::string{"server output closed before a reply"});
    }
    pending.append(buffer.data(), static_cast<std::size_t>(size));
  }
}

} // namespace detail

struct InputStep {
  std::string text;
  std::chrono::milliseconds pause_after{};
};

inline libtmux::expected<std::vector<std::string>, std::string> run_backpressure_probe(
    const std::filesystem::path& program, std::vector<std::string> arguments,
    std::vector<std::string> environment, std::string_view initialize,
    std::string_view initialized, std::string_view first_request,
    std::string_view duplicate_request, std::chrono::seconds timeout) {
  std::array<int, 2> to_child{};
  std::array<int, 2> from_child{};
  if (::pipe(to_child.data()) != 0 || ::pipe(from_child.data()) != 0) {
    return libtmux::unexpected(std::string{"pipe: "} + std::strerror(errno));
  }

  posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, to_child[1]);
  ::posix_spawn_file_actions_addclose(&actions, from_child[0]);
  std::vector<std::string> argv{program.string()};
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  auto argv_pointers = detail::writable(argv);
  auto environment_pointers = detail::writable(environment);
  pid_t child = 0;
  const int spawned = ::posix_spawn(&child, program.c_str(), &actions, nullptr,
                                    argv_pointers.data(), environment_pointers.data());
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(to_child[0]);
  ::close(from_child[1]);
  if (spawned != 0) {
    ::close(to_child[1]);
    ::close(from_child[0]);
    return libtmux::unexpected(program.string() + ": " + std::strerror(spawned));
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto abort = [&](std::string error) {
    ::close(to_child[1]);
    ::close(from_child[0]);
    static_cast<void>(::kill(child, SIGKILL));
    static_cast<void>(::waitpid(child, nullptr, 0));
    return libtmux::expected<std::vector<std::string>, std::string>{
        libtmux::unexpected(std::move(error))};
  };
  static_cast<void>(::signal(SIGPIPE, SIG_IGN));
  if (auto written = detail::write_all(to_child[1], initialize); !written.has_value()) {
    return abort(written.error());
  }
  std::string pending;
  auto initialized_reply = detail::read_line(from_child[0], pending, deadline);
  if (!initialized_reply.has_value()) {
    return abort(initialized_reply.error());
  }
  if (auto written = detail::write_all(to_child[1], initialized);
      !written.has_value()) {
    return abort(written.error());
  }
  if (auto written = detail::write_all(to_child[1], first_request);
      !written.has_value()) {
    return abort(written.error());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{250});
  if (auto written = detail::write_all(to_child[1], duplicate_request);
      !written.has_value()) {
    return abort(written.error());
  }

  auto first_reply = detail::read_line(from_child[0], pending, deadline);
  if (!first_reply.has_value()) {
    return abort(first_reply.error());
  }
  auto duplicate_reply = detail::read_line(from_child[0], pending, deadline);
  if (!duplicate_reply.has_value()) {
    return abort(duplicate_reply.error());
  }
  ::close(to_child[1]);
  ::close(from_child[0]);

  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return libtmux::unexpected(program.string() + " exited abnormally");
      }
      return std::vector<std::string>{*std::move(initialized_reply),
                                      *std::move(first_reply),
                                      *std::move(duplicate_reply)};
    }
    if (waited < 0) {
      return libtmux::unexpected(std::string{"waitpid: "} + std::strerror(errno));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  static_cast<void>(::kill(child, SIGKILL));
  static_cast<void>(::waitpid(child, nullptr, 0));
  return libtmux::unexpected(program.string() + " did not finish in time");
}

inline libtmux::expected<std::string, std::string>
run_server_steps(const std::filesystem::path& program,
                 std::vector<std::string> arguments,
                 std::vector<std::string> environment,
                 const std::vector<InputStep>& steps, std::chrono::seconds timeout) {
  std::array<int, 2> to_child{};
  std::array<int, 2> from_child{};
  if (::pipe(to_child.data()) != 0 || ::pipe(from_child.data()) != 0) {
    return libtmux::unexpected(std::string{"pipe: "} + std::strerror(errno));
  }

  posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, to_child[1]);
  ::posix_spawn_file_actions_addclose(&actions, from_child[0]);

  std::vector<std::string> argv{program.string()};
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  auto argv_pointers = detail::writable(argv);
  auto environment_pointers = detail::writable(environment);

  pid_t child = 0;
  const int spawned = ::posix_spawn(&child, program.c_str(), &actions, nullptr,
                                    argv_pointers.data(), environment_pointers.data());
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(to_child[0]);
  ::close(from_child[1]);
  if (spawned != 0) {
    ::close(to_child[1]);
    ::close(from_child[0]);
    return libtmux::unexpected(program.string() + ": " + std::strerror(spawned));
  }

  // SIGPIPE would kill the suite if the server exited early; a short write is
  // reported instead.
  static_cast<void>(::signal(SIGPIPE, SIG_IGN));
  for (const InputStep& step : steps) {
    std::size_t written = 0;
    while (written < step.text.size()) {
      const auto wrote =
          ::write(to_child[1], step.text.data() + written, step.text.size() - written);
      if (wrote <= 0) {
        break;
      }
      written += static_cast<std::size_t>(wrote);
    }
    if (step.pause_after > std::chrono::milliseconds::zero()) {
      std::this_thread::sleep_for(step.pause_after);
    }
  }
  ::close(to_child[1]);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string output;
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
      ::close(from_child[0]);
      return libtmux::unexpected(program.string() + " did not finish in time");
    }
    pollfd waiting{.fd = from_child[0], .events = POLLIN, .revents = 0};
    if (::poll(&waiting, 1, static_cast<int>(remaining.count())) <= 0) {
      continue;
    }
    std::array<char, 4096> buffer{};
    const auto read_bytes = ::read(from_child[0], buffer.data(), buffer.size());
    if (read_bytes <= 0) {
      break;
    }
    output.append(buffer.data(), static_cast<std::size_t>(read_bytes));
  }
  ::close(from_child[0]);

  int status = 0;
  if (::waitpid(child, &status, 0) < 0) {
    return libtmux::unexpected(std::string{"waitpid: "} + std::strerror(errno));
  }
  if (WIFSIGNALED(status)) {
    return libtmux::unexpected(program.string() + " was killed by signal " +
                               std::to_string(WTERMSIG(status)));
  }
  if (!WIFEXITED(status)) {
    return libtmux::unexpected(program.string() + " did not exit normally");
  }
  if (WEXITSTATUS(status) != 0) {
    return libtmux::unexpected(program.string() + " exited with status " +
                               std::to_string(WEXITSTATUS(status)));
  }
  return output;
}

// stdout only. The server's diagnostics go to stderr and are left on the
// suite's, where a failure shows them.
inline libtmux::expected<std::string, std::string>
run_server(const std::filesystem::path& program, std::vector<std::string> arguments,
           std::vector<std::string> environment, const std::string& input,
           std::chrono::seconds timeout,
           std::chrono::milliseconds linger_before_eof = {}) {
  return run_server_steps(program, std::move(arguments), std::move(environment),
                          {{input, linger_before_eof}}, timeout);
}

} // namespace libtmux::mcp::test
